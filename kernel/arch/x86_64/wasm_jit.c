// ============================================================================
// wasm_jit.c — JIT: внутренний код WebAssembly -> машинный код x86-64 (51.5).
// ----------------------------------------------------------------------------
// Интерпретатор тратит время не на сами команды, а на дорогу к ним: взять
// слово, разобрать его, прыгнуть по таблице. Здесь каждая функция модуля один
// раз, при создании экземпляра, переводится в машинный код: команда внутреннего
// кода -> заранее известная цепочка команд процессора («шаблон»). Разбор и
// прыжки по таблице исчезают, операнды адресуются напрямую.
//
// ГЛАВНОЕ РЕШЕНИЕ: машинный код живёт В ТЕХ ЖЕ ДАННЫХ, что интерпретатор —
// тот же стек значений (s->stack: fp, sp), те же кадры вызовов (s->frames), то
// же «положение» (функция, кадр, вершина стека, место во внутреннем коде).
// Родным стеком процессора машинный код не пользуется вовсе. Отсюда:
//   * пауза и продолжение (51.3) работают как раньше: положение сохраняется в
//     хранилище в том же виде, продолжить может и JIT, и интерпретатор;
//   * глубокая рекурсия программы не тратит стек ядра (он 32 КБ);
//   * всё редкое и сложное (таблицы, bulk-memory, округления, преобразования с
//     ловушками, рост памяти, функции хоста) НЕ переписано на ассемблере:
//     машинный код выходит к «водителю» (wasm_jit_run), тот просит
//     интерпретатор исполнить ровно одну команду и возвращается обратно.
// Проверок типов и глубины стека тут нет по той же причине, что в
// интерпретаторе: их доказала проверка модуля. Границы памяти и таблиц,
// деление, глубина вызовов проверяются на каждом шаге.
//
// Регистры машинного кода (все сохраняются при вызовах C, поэтому прерывание
// посреди него ничего не портит):
//   rbx — fp (локальные текущей функции)   r12 — начало памяти программы
//   r13 — sp (вершина стека значений)       rbp — её размер в байтах
//   r14 — контекст (wasm_jit_ctx_t)         r15 — счётчик витков до «тика»
//   rax, rcx, rdx, rsi, rdi, xmm0 — рабочие.
//
// «Ленивая» вершина стека: внутри прямого участка кода sp не двигается —
// эмиттер помнит сдвиг d и адресует операнды как [r13 + 8*(d - k)]. Настоящий
// r13 обновляется только на границах: метка, переход, вызов, выход.
//
// «Виртуальная» вершина стека: верхние значения не обязаны лежать в своих
// ячейках. Эмиттер помнит про каждое, где оно на самом деле: в ячейке (V_MEM),
// в локальной переменной (V_LOC — после local.get ничего копировать не надо),
// известная константа (V_CONST), в регистре rax (V_RAX — результат последней
// операции) или во флагах процессора (V_FLAGS — сравнение, за которым сразу
// идёт условный переход). Так `local.get a; local.get b; i32.add; local.set c`
// — это три команды процессора вместо двенадцати, а сравнение с переходом —
// `cmp` и `jcc`. В ячейки всё раскладывается («материализуется») там, где код
// сходится из разных мест или уходит туда, где про это не знают: метка,
// переход, вызов, команда для интерпретатора. Шаблоны, которые про это не
// знают, просто просят разложить всё заранее.
//
// Выходы к водителю — «записи выхода» (jit_exit_t): причина, функция, место
// во внутреннем коде, адрес продолжения. Машинный код кладёт адрес записи в
// rax и прыгает на общий хвост, который сохраняет регистры и возвращается.
// ============================================================================
#include "wasm_int.h"
#include <stddef.h>

enum { X_DONE = 1, X_TRAP, X_TICK, X_TICK_CALL, X_CALLOUT, X_FALLBACK, X_RETFRAME };

typedef struct jit_exit {
    uint8_t  kind;                      // X_*
    uint8_t  trap;                      // для X_TRAP
    wasm_funcinst_t* fn;                // чья это команда
    const uint64_t*  ip;                // место во внутреннем коде (см. водителя)
    void*    cont;                      // адрес продолжения в машинном коде
    uint32_t cont_off;                  // … пока код не переехал на своё место — смещение
    struct jit_exit* next;
} jit_exit_t;

enum { RAX = 0, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8, R9, R10, R11, R12, R13, R14, R15 };
enum { CC_B = 2, CC_AE = 3, CC_E = 4, CC_NE = 5, CC_BE = 6, CC_A = 7, CC_P = 10, CC_NP = 11,
       CC_L = 12, CC_GE = 13, CC_LE = 14, CC_G = 15 };

#define CTX(f)   ((int32_t)offsetof(wasm_jit_ctx_t, f))

typedef struct { uint32_t pos; uint32_t target; } fix_t;                // rel32 -> слово внутреннего кода
typedef struct { uint32_t pos; uint8_t trap; jit_exit_t* rec; void* aux; bool has_aux; } stub_t;
typedef struct { uint32_t pos; uint32_t func; } ffix_t;                 // rel32 -> вход функции модуля

enum { V_MEM = 0, V_LOC, V_CONST, V_RAX, V_FLAGS };
typedef struct { uint8_t kind; uint8_t cc; uint32_t local; uint64_t imm; } vent_t;
#define NV_MAX 16

typedef struct {
    wasm_store_t* s;
    wasm_instance_t* inst;
    uint8_t* p; uint32_t n, cap;        // машинный код (пока в обычной памяти)
    bool fail;
    int32_t d;                          // ленивый сдвиг вершины стека, в ячейках
    vent_t  vs[NV_MAX];                 // где на самом деле верхние nv значений (vs[nv-1] — вершина)
    int     nv;
    int     rax_at;                     // какая запись сейчас в rax (-1 — никакая)
    uint32_t exit_common;               // смещение общего хвоста выхода
    // текущая функция
    wasm_funcinst_t* fn;
    const uint64_t* code; uint32_t len;
    uint32_t* lab;                      // слово внутреннего кода -> смещение машинного
    uint8_t*  tgt;                      // на это слово есть переход
    fix_t*  fix;  uint32_t n_fix, cap_fix;
    stub_t* stub; uint32_t n_stub, cap_stub;
    uint32_t* rfix; uint32_t n_rfix, cap_rfix;      // rel32 -> общий хвост возврата функции
    wasm_jit_pt_t* map; uint32_t n_map, cap_map;    // точки входа этой функции (смещения)
    // на весь модуль
    ffix_t* ffix; uint32_t n_ffix, cap_ffix;
    jit_exit_t* recs;
} J;

// ---------------------------------------------------------------------------
// Память эмиттера.
// ---------------------------------------------------------------------------
static bool grow(J* j, void** arr, uint32_t* cap, uint32_t need, uint32_t item) {
    if (need <= *cap) return true;
    uint32_t nc = *cap ? *cap * 2 : 256;
    while (nc < need) nc *= 2;
    uint8_t* q = (uint8_t*)wasm_port_alloc((size_t)nc * item);
    if (!q) { j->fail = true; return false; }
    for (uint64_t i = 0; i < (uint64_t)*cap * item; i++) q[i] = ((uint8_t*)*arr)[i];
    if (*arr) wasm_port_free(*arr);
    *arr = q; *cap = nc;
    return true;
}
static void e8(J* j, uint8_t b) {
    if (j->n == j->cap && !grow(j, (void**)&j->p, &j->cap, j->n + 1, 1)) return;
    j->p[j->n++] = b;
}
static void e32(J* j, uint32_t v) { for (int i = 0; i < 4; i++) e8(j, (uint8_t)(v >> (8 * i))); }
static void e64(J* j, uint64_t v) { for (int i = 0; i < 8; i++) e8(j, (uint8_t)(v >> (8 * i))); }
static void put32(J* j, uint32_t pos, uint32_t v) {
    if (j->fail) return;
    for (int i = 0; i < 4; i++) j->p[pos + i] = (uint8_t)(v >> (8 * i));
}

// ---------------------------------------------------------------------------
// Кодирование команд x86-64.
// ---------------------------------------------------------------------------
// Общий вид: [приставка] [REX] [0F] код ModRM [SIB] [смещение].
static void rex(J* j, int w, int reg, int index, int base) {
    uint8_t r = (uint8_t)(0x40 | (w ? 8 : 0) | ((reg >> 3) << 2) | ((index >> 3) << 1) | (base >> 3));
    if (r != 0x40) e8(j, r);
}
static void head(J* j, int pfx, int w, int esc, int opc, int reg, int index, int base) {
    if (pfx) e8(j, (uint8_t)pfx);
    rex(j, w, reg, index, base);
    if (esc) e8(j, 0x0F);
    e8(j, (uint8_t)opc);
}
// Операнд в памяти [base + disp].
static void op_m(J* j, int pfx, int w, int esc, int opc, int reg, int base, int32_t disp) {
    head(j, pfx, w, esc, opc, reg, 0, base);
    int mod = (disp == 0 && (base & 7) != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    e8(j, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (base & 7)));
    if ((base & 7) == 4) e8(j, 0x24);               // rsp/r12 как база требуют байта SIB
    if (mod == 1) e8(j, (uint8_t)disp); else if (mod == 2) e32(j, (uint32_t)disp);
}
// Операнд в памяти [base + index + disp].
static void op_mx(J* j, int pfx, int w, int esc, int opc, int reg, int base, int index, int32_t disp) {
    head(j, pfx, w, esc, opc, reg, index, base);
    int mod = (disp == 0 && (base & 7) != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    e8(j, (uint8_t)((mod << 6) | ((reg & 7) << 3) | 4));
    e8(j, (uint8_t)(((index & 7) << 3) | (base & 7)));
    if (mod == 1) e8(j, (uint8_t)disp); else if (mod == 2) e32(j, (uint32_t)disp);
}
// Оба операнда — регистры (rm — второй).
static void op_r(J* j, int pfx, int w, int esc, int opc, int reg, int rm) {
    head(j, pfx, w, esc, opc, reg, 0, rm);
    e8(j, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}
static void mov_imm64(J* j, int reg, uint64_t v) {
    if (v <= 0xFFFFFFFFull) { rex(j, 0, 0, 0, reg); e8(j, (uint8_t)(0xB8 + (reg & 7))); e32(j, (uint32_t)v); }   // mov r32, imm32 обнуляет верх
    else { rex(j, 1, 0, 0, reg); e8(j, (uint8_t)(0xB8 + (reg & 7))); e64(j, v); }
}
static void mov_ptr(J* j, int reg, const void* p) { mov_imm64(j, reg, (uint64_t)(uintptr_t)p); }

// Ячейка стека значений: k-я от вершины (1 — верхняя), 0 — первая свободная.
#define SL(k)   R13, (int32_t)(8 * (j->d - (int32_t)(k)))
static void ld32(J* j, int reg, int base, int32_t disp) { op_m(j, 0, 0, 0, 0x8B, reg, base, disp); }
static void ld64(J* j, int reg, int base, int32_t disp) { op_m(j, 0, 1, 0, 0x8B, reg, base, disp); }
static void st64(J* j, int reg, int base, int32_t disp) { op_m(j, 0, 1, 0, 0x89, reg, base, disp); }

static void flush(J* j) {
    if (j->d) op_m(j, 0, 1, 0, 0x8D, R13, R13, 8 * j->d);      // lea r13, [r13 + 8*d]
    j->d = 0;
}

// ---- виртуальная вершина стека ------------------------------------------------------
#define VDISP(i)   ((int32_t)(8 * (j->d - (j->nv - (int)(i)))))        // ячейка записи i
// Положить запись в её ячейку. Рабочий регистр — rcx: rax может быть занят
// чужим значением, а флаги трогать нельзя (mov их не меняет).
static void mat(J* j, int i) {
    vent_t* e = &j->vs[i];
    switch (e->kind) {
    case V_LOC:   ld64(j, RCX, RBX, 8 * (int32_t)e->local); st64(j, RCX, R13, VDISP(i)); break;
    case V_CONST:
        if (e->imm <= 0x7FFFFFFFull) { op_m(j, 0, 1, 0, 0xC7, 0, R13, VDISP(i)); e32(j, (uint32_t)e->imm); }
        else { mov_imm64(j, RCX, e->imm); st64(j, RCX, R13, VDISP(i)); }
        break;
    case V_RAX:   st64(j, RAX, R13, VDISP(i)); j->rax_at = -1; break;
    case V_FLAGS:
        e8(j, 0x0F); e8(j, (uint8_t)(0x90 + e->cc)); e8(j, 0xC1);      // setcc cl
        e8(j, 0x0F); e8(j, 0xB6); e8(j, 0xC9);                          // movzx ecx, cl
        st64(j, RCX, R13, VDISP(i));
        break;
    default: break;
    }
    e->kind = V_MEM;
}
static void mat_all(J* j) {
    // Флаги — первыми: раскладка остальных их не портит, но так спокойнее.
    for (int i = j->nv - 1; i >= 0; i--) mat(j, i);
    j->nv = 0; j->rax_at = -1;
}
static void spill_rax(J* j) { if (j->rax_at >= 0) mat(j, j->rax_at); }
// Чтобы у шаблона было n записей: недостающие снизу — «в ячейке».
static void vneed(J* j, int n) {
    while (j->nv < n) {
        for (int i = j->nv; i > 0; i--) j->vs[i] = j->vs[i - 1];
        j->vs[0].kind = V_MEM;
        if (j->rax_at >= 0) j->rax_at++;
        j->nv++;
    }
}
static vent_t* vpush(J* j, int kind) {
    if (j->nv == NV_MAX) {
        // Глубже шестнадцати не следим: нижняя запись уходит в свою ячейку.
        // Её ячейка считается от ТЕКУЩИХ d и nv — до сдвига.
        mat(j, 0);
        for (int i = 1; i < j->nv; i++) j->vs[i - 1] = j->vs[i];
        if (j->rax_at >= 0) j->rax_at--;
        j->nv--;
    }
    vent_t* e = &j->vs[j->nv++];
    j->d++;
    e->kind = (uint8_t)kind; e->cc = 0; e->local = 0; e->imm = 0;
    if (kind == V_RAX) j->rax_at = j->nv - 1;
    return e;
}
static void vpop(J* j) {
    if (j->rax_at == j->nv - 1) j->rax_at = -1;
    j->nv--; j->d--;
}
// Значение записи — в регистр (w: 64 бита; иначе 32 с обнулением верха).
static void vload(J* j, int w, int reg, int i) {
    vent_t* e = &j->vs[i];
    switch (e->kind) {
    case V_MEM:   op_m(j, 0, w, 0, 0x8B, reg, R13, VDISP(i)); break;
    case V_LOC:   op_m(j, 0, w, 0, 0x8B, reg, RBX, 8 * (int32_t)e->local); break;
    case V_CONST: mov_imm64(j, reg, w ? e->imm : (uint32_t)e->imm); break;
    case V_RAX:   if (reg != RAX) op_r(j, 0, 1, 0, 0x89, RAX, reg); break;       // mov reg, rax
    default: break;
    }
}
// Команда с записью как вторым операндом: `op reg, [запись]` (запись — в памяти).
static void vop_m(J* j, int w, int esc, int opc, int reg, int i) {
    vent_t* e = &j->vs[i];
    if (e->kind == V_LOC) op_m(j, 0, w, esc, opc, reg, RBX, 8 * (int32_t)e->local);
    else op_m(j, 0, w, esc, opc, reg, R13, VDISP(i));
}
static bool in_mem(const vent_t* e) { return e->kind == V_MEM || e->kind == V_LOC; }
static bool imm_ok(const vent_t* e, int w) {
    return e->kind == V_CONST && (!w || (int64_t)e->imm == (int64_t)(int32_t)e->imm);
}
// Запись i — в rax (если она уже там — ничего). Прежний хозяин rax уходит в ячейку.
static void take_rax(J* j, int w, int i) {
    if (j->vs[i].kind == V_RAX) return;
    spill_rax(j);
    vload(j, w, RAX, i);
}
static int cc_swap(int cc) {            // условие при переставленных операндах
    switch (cc) {
    case CC_L: return CC_G;   case CC_G: return CC_L;   case CC_LE: return CC_GE; case CC_GE: return CC_LE;
    case CC_B: return CC_A;   case CC_A: return CC_B;   case CC_BE: return CC_AE; case CC_AE: return CC_BE;
    default: return cc;
    }
}

// Двухместная целая команда: a (вторая) op b (верхняя) -> rax.
//   opc — вид «op reg, r/m» (add 03, sub 2B, and 23, or 0B, xor 33, cmp 3B),
//   ext — номер той же команды в группе 81 /ext (с константой).
static void vbin(J* j, int w, int opc, int ext, bool commut) {
    vneed(j, 2);
    int a = j->nv - 2, b = j->nv - 1;
    if (j->vs[b].kind == V_RAX && commut && j->vs[a].kind != V_RAX) { int t = a; a = b; b = t; }
    if (j->vs[b].kind == V_RAX) {
        // Верхняя — в rax, а первой нужна вторая: верхнюю — в rcx.
        op_r(j, 0, 1, 0, 0x89, RAX, RCX);
        j->rax_at = -1;
        vload(j, w, RAX, a);
        op_r(j, 0, w, 0, opc, RAX, RCX);
    } else {
        take_rax(j, w, a);
        if (in_mem(&j->vs[b])) vop_m(j, w, 0, opc, RAX, b);
        else if (imm_ok(&j->vs[b], w)) { op_r(j, 0, w, 0, 0x81, ext, RAX); e32(j, (uint32_t)j->vs[b].imm); }
        else { vload(j, w, RCX, b); op_r(j, 0, w, 0, opc, RAX, RCX); }
    }
    j->rax_at = -1;
    vpop(j); vpop(j);
}
static void vmul(J* j, int w) {
    vneed(j, 2);
    int a = j->nv - 2, b = j->nv - 1;
    if (j->vs[b].kind == V_RAX) { int t = a; a = b; b = t; }
    take_rax(j, w, a);
    if (in_mem(&j->vs[b])) vop_m(j, w, 1, 0xAF, RAX, b);
    else { vload(j, w, RCX, b); op_r(j, 0, w, 1, 0xAF, RAX, RCX); }
    j->rax_at = -1;
    vpop(j); vpop(j);
    vpush(j, V_RAX);
}
static void vshift(J* j, int w, int ext) {
    vneed(j, 2);
    int a = j->nv - 2, b = j->nv - 1;
    if (j->vs[b].kind == V_CONST) {
        take_rax(j, w, a);
        uint8_t n = (uint8_t)(j->vs[b].imm & (w ? 63 : 31));
        op_r(j, 0, w, 0, 0xC1, ext, RAX); e8(j, n);
    } else {
        if (j->vs[b].kind == V_RAX) { op_r(j, 0, 1, 0, 0x89, RAX, RCX); j->rax_at = -1; }
        else vload(j, 0, RCX, b);
        take_rax(j, w, a);
        op_r(j, 0, w, 0, 0xD3, ext, RAX);
    }
    j->rax_at = -1;
    vpop(j); vpop(j);
    vpush(j, V_RAX);
}
// Итог сравнения: во флагах, если сразу за ним — условный переход (или
// «равно нулю», которое условие просто переворачивает); иначе — 0/1 в rax.
static void vcond(J* j, int cc, bool fuse) {
    if (fuse) { vent_t* e = vpush(j, V_FLAGS); e->cc = (uint8_t)cc; return; }
    e8(j, 0x0F); e8(j, (uint8_t)(0x90 + cc)); e8(j, 0xC0);     // setcc al
    e8(j, 0x0F); e8(j, 0xB6); e8(j, 0xC0);                      // movzx eax, al
    vpush(j, V_RAX);
}
static void vcmp(J* j, int w, int cc, bool fuse) {
    vneed(j, 2);
    int a = j->nv - 2, b = j->nv - 1;
    if (j->vs[b].kind == V_RAX) { int t = a; a = b; b = t; cc = cc_swap(cc); }
    take_rax(j, w, a);
    if (in_mem(&j->vs[b])) vop_m(j, w, 0, 0x3B, RAX, b);
    else if (imm_ok(&j->vs[b], w)) { op_r(j, 0, w, 0, 0x81, 7, RAX); e32(j, (uint32_t)j->vs[b].imm); }
    else { vload(j, w, RCX, b); op_r(j, 0, w, 0, 0x3B, RAX, RCX); }
    j->rax_at = -1;
    vpop(j); vpop(j);
    vcond(j, cc, fuse);
}
// Условие для перехода: верхняя запись -> флаги; возвращает «истина».
static int vtest(J* j) {
    vneed(j, 1);
    int t = j->nv - 1, cc = CC_NE;
    vent_t* e = &j->vs[t];
    if (e->kind == V_FLAGS) cc = e->cc;
    else if (in_mem(e)) { if (e->kind == V_LOC) op_m(j, 0, 0, 0, 0x83, 7, RBX, 8 * (int32_t)e->local); else op_m(j, 0, 0, 0, 0x83, 7, R13, VDISP(t)); e8(j, 0); }
    else { take_rax(j, 0, t); e8(j, 0x85); e8(j, 0xC0); }       // test eax, eax
    vpop(j);
    return cc;
}

// ---- переходы -------------------------------------------------------------------
static uint32_t jcc_fwd(J* j, int cc) { e8(j, 0x0F); e8(j, (uint8_t)(0x80 + cc)); e32(j, 0); return j->n - 4; }
static uint32_t jmp_fwd(J* j) { e8(j, 0xE9); e32(j, 0); return j->n - 4; }
static void here(J* j, uint32_t pos) { put32(j, pos, j->n - (pos + 4)); }
static void to_off(J* j, uint32_t pos, uint32_t off) { put32(j, pos, off - (pos + 4)); }

static void fix_add(J* j, uint32_t pos, uint32_t target) {
    if (!grow(j, (void**)&j->fix, &j->cap_fix, j->n_fix + 1, sizeof(fix_t))) return;
    j->fix[j->n_fix].pos = pos; j->fix[j->n_fix].target = target; j->n_fix++;
}
static void jmp_ir(J* j, uint32_t target) { fix_add(j, jmp_fwd(j), target); }
static void jcc_ir(J* j, int cc, uint32_t target) { fix_add(j, jcc_fwd(j, cc), target); }

static jit_exit_t* rec_new(J* j, int kind, int trap, const uint64_t* ip) {
    jit_exit_t* r = (jit_exit_t*)wasm_blk_alloc(&j->s->arena, sizeof(*r), 0);
    if (!r) { j->fail = true; return 0; }
    r->kind = (uint8_t)kind; r->trap = (uint8_t)trap; r->fn = j->fn; r->ip = ip;
    r->next = j->recs; j->recs = r;
    return r;
}
static void stub_add(J* j, uint32_t pos, int trap, jit_exit_t* rec, void* aux, bool has_aux) {
    if (!grow(j, (void**)&j->stub, &j->cap_stub, j->n_stub + 1, sizeof(stub_t))) return;
    stub_t* st = &j->stub[j->n_stub++];
    st->pos = pos; st->trap = (uint8_t)trap; st->rec = rec; st->aux = aux; st->has_aux = has_aux;
}
// Условный переход на ловушку: сами выходы — в конце функции, по одному на вид.
static void jcc_trap(J* j, int cc, int trap) { stub_add(j, jcc_fwd(j, cc), trap, 0, 0, false); }
static void jcc_exit(J* j, int cc, jit_exit_t* rec) { stub_add(j, jcc_fwd(j, cc), 0, rec, 0, false); }

static void map_add(J* j, const uint64_t* ip, uint32_t off) {
    if (!grow(j, (void**)&j->map, &j->cap_map, j->n_map + 1, sizeof(wasm_jit_pt_t))) return;
    j->map[j->n_map].ip = ip; j->map[j->n_map].native = (void*)(uintptr_t)off; j->n_map++;
}

// «Тик»: счётчик витков. Вышел — к водителю (часы, флаг остановки, «топливо»,
// конец отрезка); продолжение — сразу за проверкой.
static void tick(J* j, jit_exit_t* rec, void* aux, bool has_aux) {
    e8(j, 0x41); e8(j, 0x83); e8(j, 0xEF); e8(j, 0x01);         // sub r15d, 1
    stub_add(j, jcc_fwd(j, CC_LE), 0, rec, aux, has_aux);
    if (rec) rec->cont_off = j->n;
}

// Указатель и размер памяти программы — в регистры. Память могла вырасти и
// переехать, а вызванная функция — быть из другого экземпляра: после вызова и
// на входе в функцию берём заново.
static void reload_mem(J* j) {
    if (!j->inst->mem) return;
    mov_ptr(j, RAX, j->inst->mem);
    ld64(j, R12, RAX, (int32_t)offsetof(wasm_memory_t, data));
    ld64(j, RBP, RAX, (int32_t)offsetof(wasm_memory_t, bytes));
}

// Перенос keep верхних значений вниз на drop ячеек и переход (sp уже настоящий).
static void do_br(J* j, uint32_t target, uint32_t keep, uint32_t drop) {
    if (drop) {
        for (uint32_t i = 0; i < keep; i++) {
            ld64(j, RAX, R13, 8 * ((int32_t)i - (int32_t)keep));
            st64(j, RAX, R13, 8 * ((int32_t)i - (int32_t)keep - (int32_t)drop));
        }
        op_m(j, 0, 1, 0, 0x8D, R13, R13, -8 * (int32_t)drop);
    }
    jmp_ir(j, target);
}

// Кадр вызова и переход в вызываемую. np — число её параметров; ip_after —
// куда вернётся интерпретатор, если продолжать придётся ему. Вызываемая — либо
// известна заранее (func >= 0: функция этого модуля, переход по смещению), либо
// её вход уже в rdx.
static void push_frame_and_go(J* j, const uint64_t* ip_after, uint32_t np, int64_t func) {
    ld64(j, RCX, R14, CTX(ftop));
    op_m(j, 0, 1, 0, 0x3B, RCX, R14, CTX(fend));                // cmp rcx, [fend]
    jcc_trap(j, CC_AE, WASM_TRAP_STACK);
    mov_ptr(j, RAX, ip_after);
    st64(j, RAX, RCX, (int32_t)offsetof(wasm_frame_t, ip));
    st64(j, RBX, RCX, (int32_t)offsetof(wasm_frame_t, fp));
    mov_ptr(j, RAX, j->fn);
    st64(j, RAX, RCX, (int32_t)offsetof(wasm_frame_t, fn));
    // lea rax, [rip + адрес возврата]; его узнаем, когда допишем переход
    e8(j, 0x48); e8(j, 0x8D); e8(j, 0x05); e32(j, 0);
    uint32_t lea_pos = j->n - 4;
    st64(j, RAX, RCX, (int32_t)offsetof(wasm_frame_t, native));
    e8(j, 0x48); e8(j, 0x83); e8(j, 0xC1); e8(j, (uint8_t)sizeof(wasm_frame_t));    // add rcx, размер кадра
    st64(j, RCX, R14, CTX(ftop));
    op_m(j, 0, 1, 0, 0x8D, RBX, R13, -8 * (int32_t)np);         // fp вызываемой = sp - параметры
    if (func >= 0) {
        uint32_t pos = jmp_fwd(j);
        if (grow(j, (void**)&j->ffix, &j->cap_ffix, j->n_ffix + 1, sizeof(ffix_t))) {
            j->ffix[j->n_ffix].pos = pos; j->ffix[j->n_ffix].func = (uint32_t)func; j->n_ffix++;
        }
    } else {
        e8(j, 0xFF); e8(j, 0xE2);                               // jmp rdx
    }
    here(j, lea_pos);                                           // сюда вернётся вызываемая
    map_add(j, ip_after, j->n);
    reload_mem(j);
}

// Команда, которую исполнит интерпретатор: выйти к водителю, вернуться сюда же.
static void fallback(J* j, uint32_t i, uint32_t next) {
    flush(j);
    jit_exit_t* r = rec_new(j, X_FALLBACK, 0, j->code + i);
    mov_ptr(j, RAX, r);
    uint32_t pos = jmp_fwd(j);
    to_off(j, pos, j->exit_common);
    if (r) r->cont_off = j->n;
    map_add(j, j->code + next, j->n);
}

// ---- шаблоны числовых команд -----------------------------------------------------
// Деление. Процессор на ноль и на INT_MIN / -1 отвечает исключением —
// проверяем сами: первое — ловушка, второе — ловушка у div и ноль у rem.
static void divide(J* j, int w, bool sign, bool rem) {
    op_m(j, 0, w, 0, 0x8B, RCX, SL(1));
    op_r(j, 0, w, 0, 0x85, RCX, RCX);                           // test rcx, rcx
    jcc_trap(j, CC_E, WASM_TRAP_DIV_ZERO);
    op_m(j, 0, w, 0, 0x8B, RAX, SL(2));
    uint32_t done = 0; bool has_done = false;
    if (sign) {
        rex(j, w, 0, 0, RCX); e8(j, 0x83); e8(j, 0xF9); e8(j, 0xFF);        // cmp rcx, -1
        uint32_t ok = jcc_fwd(j, CC_NE);
        if (rem) {
            e8(j, 0x31); e8(j, 0xD2);                           // xor edx, edx: остаток от деления на -1 — ноль
            done = jmp_fwd(j); has_done = true;
        } else {
            if (w) { mov_imm64(j, RDX, 0x8000000000000000ull); op_r(j, 0, 1, 0, 0x39, RDX, RAX); }
            else { e8(j, 0x3D); e32(j, 0x80000000u); }          // cmp eax, INT_MIN
            jcc_trap(j, CC_E, WASM_TRAP_INT_OVERFLOW);
        }
        here(j, ok);
        if (w) e8(j, 0x48);
        e8(j, 0x99);                                            // cdq / cqo
        op_r(j, 0, w, 0, 0xF7, 7, RCX);                         // idiv
    } else {
        e8(j, 0x31); e8(j, 0xD2);                               // xor edx, edx
        op_r(j, 0, w, 0, 0xF7, 6, RCX);                         // div
    }
    if (has_done) here(j, done);
    st64(j, rem ? RDX : RAX, SL(2));
    j->d--;
}
// Адрес в памяти программы: индекс (верхняя ячейка минус skip) + смещение;
// за границей — ловушка. Возвращает регистр-индекс и смещение для [r12 + reg + disp].
static void mem_addr(J* j, uint32_t off, uint32_t n, int* reg, int32_t* disp) {
    if ((uint64_t)off + n <= 0x7FFFFFFFull) {
        op_m(j, 0, 1, 0, 0x8D, RCX, RAX, (int32_t)(off + n));  // lea rcx, [rax + off + n]
        op_r(j, 0, 1, 0, 0x39, RBP, RCX);                       // cmp rcx, rbp
        jcc_trap(j, CC_A, WASM_TRAP_MEM_OOB);
        *reg = RAX; *disp = (int32_t)off;
    } else {
        mov_imm64(j, RCX, off);
        op_r(j, 0, 1, 0, 0x01, RAX, RCX);                       // add rcx, rax
        op_m(j, 0, 1, 0, 0x8D, RSI, RCX, (int32_t)n);          // rsi: в rdx может лежать значение для записи
        op_r(j, 0, 1, 0, 0x39, RBP, RSI);
        jcc_trap(j, CC_A, WASM_TRAP_MEM_OOB);
        *reg = RCX; *disp = 0;
    }
}
static void load(J* j, uint64_t w, int pfx, int rw, int esc, int opc, uint32_t n) {
    int reg; int32_t disp;
    (void)pfx;
    vneed(j, 1);
    take_rax(j, 0, j->nv - 1);                                  // индекс: 32 бита, верх — ноль
    j->rax_at = -1;
    vpop(j);
    mem_addr(j, IR_B(w), n, &reg, &disp);
    op_mx(j, 0, rw, esc, opc, RAX, R12, reg, disp);
    vpush(j, V_RAX);
}
static void store(J* j, uint64_t w, int pfx, int rw, int opc, uint32_t n) {
    int reg; int32_t disp;
    vneed(j, 2);
    // Значение — в rdx (его младший байт, dl, адресуется без REX), индекс — в rax.
    if (j->vs[j->nv - 1].kind == V_RAX) { op_r(j, 0, 1, 0, 0x89, RAX, RDX); j->rax_at = -1; }
    else vload(j, 1, RDX, j->nv - 1);
    take_rax(j, 0, j->nv - 2);
    j->rax_at = -1;
    vpop(j); vpop(j);
    mem_addr(j, IR_B(w), n, &reg, &disp);
    op_mx(j, pfx, rw, 0, opc, RDX, R12, reg, disp);
}
// Дробные: xmm0 = вторая; xmm0 op= верхняя; результат — на место второй.
static void fbin(J* j, bool dbl, int opc) {
    int p = dbl ? 0xF2 : 0xF3;
    op_m(j, p, 0, 1, 0x10, 0, SL(2));
    op_m(j, p, 0, 1, opc, 0, SL(1));
    op_m(j, p, 0, 1, 0x11, 0, SL(2));
    j->d--;
}
// Сравнение дробных. NaN не равен и не упорядочен ни с чем: «меньше» считаем
// как «больше» с переставленными операндами — тогда хватает одного флага.
static void fcmp(J* j, bool dbl, int kind) {        // 0 eq, 1 ne, 2 lt, 3 gt, 4 le, 5 ge
    int p = dbl ? 0xF2 : 0xF3;
    bool swap = (kind == 2 || kind == 4);
    op_m(j, p, 0, 1, 0x10, 0, R13, 8 * (j->d - (swap ? 1 : 2)));
    op_m(j, dbl ? 0x66 : 0, 0, 1, 0x2E, 0, R13, 8 * (j->d - (swap ? 2 : 1)));   // ucomis xmm0, другая
    if (kind == 0) {
        e8(j, 0x0F); e8(j, 0x94); e8(j, 0xC0);                  // sete al
        e8(j, 0x0F); e8(j, 0x9B); e8(j, 0xC1);                  // setnp cl
        e8(j, 0x20); e8(j, 0xC8);                               // and al, cl
    } else if (kind == 1) {
        e8(j, 0x0F); e8(j, 0x95); e8(j, 0xC0);                  // setne al
        e8(j, 0x0F); e8(j, 0x9A); e8(j, 0xC1);                  // setp cl
        e8(j, 0x08); e8(j, 0xC8);                               // or al, cl
    } else {
        e8(j, 0x0F); e8(j, (uint8_t)(0x90 + (kind <= 3 ? CC_A : CC_AE))); e8(j, 0xC0);
    }
    e8(j, 0x0F); e8(j, 0xB6); e8(j, 0xC0);
    st64(j, RAX, SL(2));
    j->d--;
}
// Результат одинарной точности из xmm0 — в ячейку целиком (верх обнулён).
static void st_f32(J* j, int k) {
    e8(j, 0x66); e8(j, 0x0F); e8(j, 0x7E); e8(j, 0xC0);         // movd eax, xmm0
    st64(j, RAX, R13, 8 * (j->d - k));
}

static uint32_t ir_len(const uint64_t* code, uint32_t i) {
    switch (IR_OP(code[i])) {
    case IR_BR: case IR_BR_IF: case IR_CONST64: return 2;
    case IR_BR_TABLE: return 1 + 2 * (IR_B(code[i]) + 1);
    default: return 1;
    }
}

// ---------------------------------------------------------------------------
// Одна функция.
// ---------------------------------------------------------------------------
static void compile_func(J* j, wasm_funcinst_t* fn, const uint64_t* code, uint32_t len, uint32_t* entry_off) {
    wasm_instance_t* inst = j->inst;
    j->fn = fn; j->code = code; j->len = len;
    j->n_fix = 0; j->n_stub = 0; j->n_rfix = 0; j->n_map = 0; j->d = 0;
    j->nv = 0; j->rax_at = -1;
    j->lab = (uint32_t*)wasm_port_alloc((size_t)(len + 1) * sizeof(uint32_t));
    j->tgt = (uint8_t*)wasm_port_alloc((size_t)len + 1);
    if (!j->lab || !j->tgt) { j->fail = true; return; }
    for (uint32_t i = 0; i <= len; i++) { j->lab[i] = 0xFFFFFFFFu; j->tgt[i] = 0; }
    // Куда ведут переходы: перед такими словами вершина стека обязана быть настоящей.
    for (uint32_t i = 0; i < len; i += ir_len(code, i)) {
        uint64_t w = code[i];
        uint32_t op = IR_OP(w), t = IR_B(w);
        if (op == IR_BR || op == IR_BR_IF || op == IR_BR_IFZ) { if (t <= len) j->tgt[t] = 1; }
        else if (op == IR_BR_TABLE) {
            for (uint32_t k = 0; k <= t; k++) { uint32_t tt = IR_B(code[i + 1 + 2 * k]); if (tt <= len) j->tgt[tt] = 1; }
        }
    }

    // ---- вход: хватит ли стека, обнулить локальные ----
    *entry_off = j->n;
    uint64_t need = (uint64_t)fn->n_params + fn->n_locals + fn->max_stack;
    mov_imm64(j, RAX, need * 8);
    op_r(j, 0, 1, 0, 0x01, RBX, RAX);                           // add rax, rbx
    op_m(j, 0, 1, 0, 0x3B, RAX, R14, CTX(stack_end));
    jcc_trap(j, CC_A, WASM_TRAP_STACK);
    if (fn->n_locals) {
        op_m(j, 0, 1, 0, 0x8D, RDI, RBX, 8 * (int32_t)fn->n_params);
        e8(j, 0x31); e8(j, 0xC0);                               // xor eax, eax
        mov_imm64(j, RCX, fn->n_locals);
        e8(j, 0xF3); e8(j, 0x48); e8(j, 0xAB);                  // rep stosq
    }
    op_m(j, 0, 1, 0, 0x8D, R13, RBX, 8 * (int32_t)(fn->n_params + fn->n_locals));
    reload_mem(j);
    map_add(j, code, j->n);                                     // вход «после пролога» — для водителя

    for (uint32_t i = 0; i < len && !j->fail;) {
        uint64_t w = code[i];
        uint32_t op = IR_OP(w), b = IR_B(w);
        uint32_t next = i + ir_len(code, i);
        // Про виртуальную вершину стека знают только частые команды (lazy);
        // остальным всё раскладывается по ячейкам заранее. Метка — место, где
        // код сходится: к ней всё разложено и вершина настоящая.
        bool lazy = op == IR_LOCAL_GET || op == IR_LOCAL_SET || op == IR_LOCAL_TEE || op == IR_CONST32 ||
                    op == IR_CONST64 || op == IR_DROP || op == IR_GLOBAL_GET || op == IR_GLOBAL_SET ||
                    op == IR_BR_IF || op == IR_BR_IFZ || (op >= 0x28 && op <= 0x3E) ||
                    (op >= 0x45 && op <= 0x5A) || op == 0x6A || op == 0x6B || op == 0x6C ||
                    (op >= 0x71 && op <= 0x78) || op == 0x7C || op == 0x7D || op == 0x7E ||
                    (op >= 0x83 && op <= 0x8A) || op == 0xA7 || op == 0xAC || op == 0xAD;
        if (j->tgt[i] || !lazy) mat_all(j);
        if (j->tgt[i]) flush(j);
        j->lab[i] = j->n;
        // Сравнение сливается с переходом, только если переход — следующее же
        // слово и на него самого никто не прыгает.
        bool fuse = next < len && !j->tgt[next] &&
                    (IR_OP(code[next]) == IR_BR_IF || IR_OP(code[next]) == IR_BR_IFZ || IR_OP(code[next]) == 0x45);
        switch (op) {
        // ---- управление ----
        case IR_UNREACHABLE: { uint32_t pos = jmp_fwd(j); stub_add(j, pos, WASM_TRAP_UNREACHABLE, 0, 0, false); break; }
        case IR_LOOP:
            flush(j);
            tick(j, rec_new(j, X_TICK, 0, code + next), 0, false);
            map_add(j, code + next, j->n);
            break;
        case IR_BR_IFZ: {
            int cc = vtest(j);
            mat_all(j); flush(j);                               // mov и lea флагов не меняют
            jcc_ir(j, cc ^ 1, b);                               // переход, если условие ложно
            break;
        }
        case IR_BR: {
            uint64_t kd = code[i + 1];
            flush(j);
            do_br(j, b, (uint32_t)kd, (uint32_t)(kd >> 32));
            break;
        }
        case IR_BR_IF: {
            uint64_t kd = code[i + 1];
            uint32_t keep = (uint32_t)kd, drop = (uint32_t)(kd >> 32);
            int cc = vtest(j);
            mat_all(j); flush(j);                               // mov и lea флагов не меняют
            if (!drop) jcc_ir(j, cc, b);
            else { uint32_t skip = jcc_fwd(j, cc ^ 1); do_br(j, b, keep, drop); here(j, skip); }
            break;
        }
        case IR_BR_TABLE: {
            ld32(j, RAX, SL(1));
            j->d--; flush(j);
            mov_imm64(j, RCX, b);
            e8(j, 0x39); e8(j, 0xC8);                           // cmp eax, ecx
            e8(j, 0x0F); e8(j, 0x43); e8(j, 0xC1);              // cmovae eax, ecx: за пределами — метка по умолчанию
            e8(j, 0x48); e8(j, 0x8D); e8(j, 0x0D); e32(j, 0);   // lea rcx, [rip + таблица]
            uint32_t lea_pos = j->n - 4;
            e8(j, 0x48); e8(j, 0x63); e8(j, 0x04); e8(j, 0x81); // movsxd rax, [rcx + rax*4]
            e8(j, 0x48); e8(j, 0x01); e8(j, 0xC8);              // add rax, rcx
            e8(j, 0xFF); e8(j, 0xE0);                           // jmp rax
            here(j, lea_pos);
            uint32_t tbl = j->n;
            for (uint32_t k = 0; k <= b; k++) e32(j, 0);
            for (uint32_t k = 0; k <= b && !j->fail; k++) {
                const uint64_t* e = code + i + 1 + 2 * k;
                put32(j, tbl + 4 * k, j->n - tbl);
                do_br(j, IR_B(e[0]), (uint32_t)e[0], (uint32_t)e[1]);
            }
            break;
        }
        case IR_RETURN: {
            uint32_t n = fn->n_results;
            for (uint32_t k = 0; k < n; k++) {
                ld64(j, RAX, R13, 8 * (j->d - (int32_t)n + (int32_t)k));
                st64(j, RAX, RBX, 8 * (int32_t)k);
            }
            uint32_t pos = jmp_fwd(j);
            if (grow(j, (void**)&j->rfix, &j->cap_rfix, j->n_rfix + 1, sizeof(uint32_t))) j->rfix[j->n_rfix++] = pos;
            break;
        }
        case IR_CALL: {
            wasm_funcinst_t* callee = inst->funcs[b];
            flush(j);
            tick(j, rec_new(j, X_TICK_CALL, 0, code + next), callee, true);
            if (callee->code && callee->inst == inst) {
                // Функция этого же модуля: её вход известен при сборке.
                push_frame_and_go(j, code + next, callee->n_params, (int64_t)b - (int64_t)inst->module->n_imp_funcs);
            } else if (callee->code && callee->jit) {
                mov_ptr(j, RDX, callee->jit);
                push_frame_and_go(j, code + next, callee->n_params, -1);
            } else {
                // Функция хоста: её исполнит водитель (или попросит об услуге).
                jit_exit_t* r = rec_new(j, X_CALLOUT, 0, code + next);
                mov_ptr(j, RAX, callee);
                st64(j, RAX, R14, CTX(aux));
                mov_ptr(j, RAX, r);
                to_off(j, jmp_fwd(j), j->exit_common);
                if (r) r->cont_off = j->n;
                map_add(j, code + next, j->n);
            }
            break;
        }
        case IR_CALL_INDIRECT: {
            const wasm_functype_t* ft = &inst->module->types[b];
            ld32(j, RAX, SL(1));
            j->d--; flush(j);
            mov_ptr(j, RCX, inst->tables[IR_A(w)]);
            op_m(j, 0, 0, 0, 0x3B, RAX, RCX, (int32_t)offsetof(wasm_table_t, size));
            jcc_trap(j, CC_AE, WASM_TRAP_UNDEF_ELEM);
            ld64(j, RCX, RCX, (int32_t)offsetof(wasm_table_t, elems));
            e8(j, 0x48); e8(j, 0x8B); e8(j, 0x04); e8(j, 0xC1); // mov rax, [rcx + rax*8]
            op_r(j, 0, 1, 0, 0x85, RAX, RAX);
            jcc_trap(j, CC_E, WASM_TRAP_NULL_ELEM);
            // Тип сверяется на ходу: что лежит в ячейке таблицы, заранее неизвестно.
            op_m(j, 0, 0, 0, 0x81, 7, RAX, (int32_t)offsetof(wasm_funcinst_t, canon)); e32(j, inst->canon[b]);
            jcc_trap(j, CC_NE, WASM_TRAP_SIG_MISMATCH);
            st64(j, RAX, R14, CTX(aux));                        // вызываемую помнит контекст: регистры выход не переживут
            tick(j, rec_new(j, X_TICK_CALL, 0, code + next), 0, false);
            ld64(j, RAX, R14, CTX(aux));
            ld64(j, RDX, RAX, (int32_t)offsetof(wasm_funcinst_t, jit));
            op_r(j, 0, 1, 0, 0x85, RDX, RDX);
            jit_exit_t* r = rec_new(j, X_CALLOUT, 0, code + next);
            jcc_exit(j, CC_E, r);                               // машинного кода нет — функция хоста
            uint32_t n0 = j->n_map;
            push_frame_and_go(j, code + next, ft->n_params, -1);
            if (r && j->n_map > n0) r->cont_off = (uint32_t)(uintptr_t)j->map[j->n_map - 1].native;
            break;
        }

        // ---- стек, локальные, глобальные ----
        case IR_DROP: vneed(j, 1); vpop(j); break;
        case IR_SELECT:
            ld64(j, RAX, SL(3));
            op_m(j, 0, 0, 0, 0x83, 7, SL(1)); e8(j, 0);
            op_m(j, 0, 1, 1, 0x44, RAX, SL(2));                 // cmove rax, [вторая]
            st64(j, RAX, SL(3));
            j->d -= 2;
            break;
        case IR_LOCAL_GET: { vent_t* e = vpush(j, V_LOC); e->local = b; break; }
        case IR_LOCAL_SET: case IR_LOCAL_TEE: {
            vneed(j, 1);
            int t = j->nv - 1;
            // Записи «значение — в этой локальной» ниже по стеку помнят СТАРОЕ
            // значение: перед записью они уходят в свои ячейки.
            for (int q = 0; q < t; q++) if (j->vs[q].kind == V_LOC && j->vs[q].local == b) mat(j, q);
            vent_t* e = &j->vs[t];
            int32_t ld = 8 * (int32_t)b;
            if (e->kind == V_RAX) st64(j, RAX, RBX, ld);
            else if (e->kind == V_CONST && e->imm <= 0x7FFFFFFFull) { op_m(j, 0, 1, 0, 0xC7, 0, RBX, ld); e32(j, (uint32_t)e->imm); }
            else if (!(e->kind == V_LOC && e->local == b)) { vload(j, 1, RCX, t); st64(j, RCX, RBX, ld); }
            if (op == IR_LOCAL_SET) vpop(j);
            break;
        }
        case IR_GLOBAL_GET:
            spill_rax(j);
            mov_ptr(j, RAX, inst->globals[b]);
            ld64(j, RAX, RAX, (int32_t)offsetof(wasm_globalinst_t, value));
            vpush(j, V_RAX);
            break;
        case IR_GLOBAL_SET:
            vneed(j, 1);
            mov_ptr(j, RDX, inst->globals[b]);
            if (j->vs[j->nv - 1].kind == V_RAX) st64(j, RAX, RDX, (int32_t)offsetof(wasm_globalinst_t, value));
            else { vload(j, 1, RCX, j->nv - 1); st64(j, RCX, RDX, (int32_t)offsetof(wasm_globalinst_t, value)); }
            vpop(j);
            break;
        case IR_CONST32: { vent_t* e = vpush(j, V_CONST); e->imm = b; break; }
        case IR_CONST64: { vent_t* e = vpush(j, V_CONST); e->imm = code[i + 1]; break; }
        case IR_REF_FUNC: mov_ptr(j, RAX, inst->funcs[b]); st64(j, RAX, SL(0)); j->d++; break;

        // ---- память ----
        case 0x28: case 0x2A: case 0x35: load(j, w, 0, 0, 0, 0x8B, 4); break;   // i32/f32.load, i64.load32_u
        case 0x29: case 0x2B: load(j, w, 0, 1, 0, 0x8B, 8); break;
        case 0x2C: load(j, w, 0, 0, 1, 0xBE, 1); break;         // movsx eax, byte
        case 0x2D: case 0x31: load(j, w, 0, 0, 1, 0xB6, 1); break;
        case 0x2E: load(j, w, 0, 0, 1, 0xBF, 2); break;
        case 0x2F: case 0x33: load(j, w, 0, 0, 1, 0xB7, 2); break;
        case 0x30: load(j, w, 0, 1, 1, 0xBE, 1); break;
        case 0x32: load(j, w, 0, 1, 1, 0xBF, 2); break;
        case 0x34: load(j, w, 0, 1, 0, 0x63, 4); break;         // movsxd
        case 0x36: case 0x38: case 0x3E: store(j, w, 0, 0, 0x89, 4); break;
        case 0x37: case 0x39: store(j, w, 0, 1, 0x89, 8); break;
        case 0x3A: case 0x3C: store(j, w, 0, 0, 0x88, 1); break;
        case 0x3B: case 0x3D: store(j, w, 0x66, 0, 0x89, 2); break;
        case IR_MEMORY_SIZE:
            mov_ptr(j, RAX, inst->mem);
            ld32(j, RAX, RAX, (int32_t)offsetof(wasm_memory_t, pages));
            st64(j, RAX, SL(0)); j->d++;
            break;

        // ---- сравнения целых ----
        case 0x45: case 0x50: {
            int W = op == 0x50;
            vneed(j, 1);
            int t = j->nv - 1;
            vent_t* e = &j->vs[t];
            int cc = CC_E;
            if (e->kind == V_FLAGS) cc = e->cc ^ 1;             // «равно нулю» от сравнения — обратное условие
            else if (in_mem(e)) { if (e->kind == V_LOC) op_m(j, 0, W, 0, 0x83, 7, RBX, 8 * (int32_t)e->local); else op_m(j, 0, W, 0, 0x83, 7, R13, VDISP(t)); e8(j, 0); }
            else { take_rax(j, W, t); op_r(j, 0, W, 0, 0x85, RAX, RAX); }
            j->rax_at = j->rax_at == t ? -1 : j->rax_at;
            vpop(j);
            if (!fuse) spill_rax(j);                            // итог пойдёт в rax
            vcond(j, cc, fuse);
            break;
        }
        case 0x46: case 0x51: vcmp(j, op >= 0x50, CC_E, fuse); break;
        case 0x47: case 0x52: vcmp(j, op >= 0x50, CC_NE, fuse); break;
        case 0x48: case 0x53: vcmp(j, op >= 0x50, CC_L, fuse); break;
        case 0x49: case 0x54: vcmp(j, op >= 0x50, CC_B, fuse); break;
        case 0x4A: case 0x55: vcmp(j, op >= 0x50, CC_G, fuse); break;
        case 0x4B: case 0x56: vcmp(j, op >= 0x50, CC_A, fuse); break;
        case 0x4C: case 0x57: vcmp(j, op >= 0x50, CC_LE, fuse); break;
        case 0x4D: case 0x58: vcmp(j, op >= 0x50, CC_BE, fuse); break;
        case 0x4E: case 0x59: vcmp(j, op >= 0x50, CC_GE, fuse); break;
        case 0x4F: case 0x5A: vcmp(j, op >= 0x50, CC_AE, fuse); break;
        // ---- сравнения дробных ----
        case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: case 0x60: fcmp(j, false, (int)op - 0x5B); break;
        case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: fcmp(j, true, (int)op - 0x61); break;

        // ---- целые ----
        case 0x67: case 0x79: {                                 // clz: 31 - (номер старшей единицы); у нуля — ширина
            int W = op == 0x79;
            op_m(j, 0, W, 0, 0x8B, RAX, SL(1));
            op_r(j, 0, W, 1, 0xBD, RAX, RAX);                   // bsr
            mov_imm64(j, RCX, W ? 127 : 63);
            op_r(j, 0, W, 1, 0x44, RAX, RCX);                   // cmove
            rex(j, W, 0, 0, RAX); e8(j, 0x83); e8(j, 0xF0); e8(j, W ? 63 : 31);   // xor rax, 63/31
            st64(j, RAX, SL(1));
            break;
        }
        case 0x68: case 0x7A: {                                 // ctz
            int W = op == 0x7A;
            op_m(j, 0, W, 0, 0x8B, RAX, SL(1));
            op_r(j, 0, W, 1, 0xBC, RAX, RAX);                   // bsf
            mov_imm64(j, RCX, W ? 64 : 32);
            op_r(j, 0, W, 1, 0x44, RAX, RCX);
            st64(j, RAX, SL(1));
            break;
        }
        case 0x6A: case 0x7C: vbin(j, op == 0x7C, 0x03, 0, true); vpush(j, V_RAX); break;
        case 0x6B: case 0x7D: vbin(j, op == 0x7D, 0x2B, 5, false); vpush(j, V_RAX); break;
        case 0x6C: case 0x7E: vmul(j, op == 0x7E); break;
        case 0x6D: divide(j, 0, true, false); break;
        case 0x6E: divide(j, 0, false, false); break;
        case 0x6F: divide(j, 0, true, true); break;
        case 0x70: divide(j, 0, false, true); break;
        case 0x7F: divide(j, 1, true, false); break;
        case 0x80: divide(j, 1, false, false); break;
        case 0x81: divide(j, 1, true, true); break;
        case 0x82: divide(j, 1, false, true); break;
        case 0x71: case 0x83: vbin(j, op == 0x83, 0x23, 4, true); vpush(j, V_RAX); break;
        case 0x72: case 0x84: vbin(j, op == 0x84, 0x0B, 1, true); vpush(j, V_RAX); break;
        case 0x73: case 0x85: vbin(j, op == 0x85, 0x33, 6, true); vpush(j, V_RAX); break;
        case 0x74: case 0x86: vshift(j, op == 0x86, 4); break;  // shl
        case 0x75: case 0x87: vshift(j, op == 0x87, 7); break;  // sar
        case 0x76: case 0x88: vshift(j, op == 0x88, 5); break;  // shr
        case 0x77: case 0x89: vshift(j, op == 0x89, 0); break;  // rol
        case 0x78: case 0x8A: vshift(j, op == 0x8A, 1); break;  // ror

        // ---- дробные ---- (abs и neg — над битами: NaN они трогать не должны)
        case 0x8B: op_m(j, 0, 0, 0, 0x81, 4, SL(1)); e32(j, 0x7FFFFFFFu); break;
        case 0x8C: op_m(j, 0, 0, 0, 0x81, 6, SL(1)); e32(j, 0x80000000u); break;
        case 0x99: op_m(j, 0, 1, 1, 0xBA, 6, SL(1)); e8(j, 63); break;      // btr qword, 63
        case 0x9A: op_m(j, 0, 1, 1, 0xBA, 7, SL(1)); e8(j, 63); break;      // btc
        case 0x91: op_m(j, 0xF3, 0, 1, 0x51, 0, SL(1)); op_m(j, 0xF3, 0, 1, 0x11, 0, SL(1)); break;
        case 0x9F: op_m(j, 0xF2, 0, 1, 0x51, 0, SL(1)); op_m(j, 0xF2, 0, 1, 0x11, 0, SL(1)); break;
        case 0x92: fbin(j, false, 0x58); break;
        case 0x93: fbin(j, false, 0x5C); break;
        case 0x94: fbin(j, false, 0x59); break;
        case 0x95: fbin(j, false, 0x5E); break;
        case 0xA0: fbin(j, true, 0x58); break;
        case 0xA1: fbin(j, true, 0x5C); break;
        case 0xA2: fbin(j, true, 0x59); break;
        case 0xA3: fbin(j, true, 0x5E); break;

        // ---- преобразования ----
        case 0xA7: case 0xAD:                                   // wrap, extend_u: верх — в ноль
            vneed(j, 1);
            if (j->vs[j->nv - 1].kind == V_CONST) { j->vs[j->nv - 1].imm &= 0xFFFFFFFFull; break; }
            if (j->vs[j->nv - 1].kind == V_RAX) { e8(j, 0x89); e8(j, 0xC0); break; }    // mov eax, eax
            take_rax(j, 0, j->nv - 1);
            vpop(j); vpush(j, V_RAX);
            break;
        case 0xAC:                                              // i64.extend_i32_s
            vneed(j, 1);
            take_rax(j, 0, j->nv - 1);
            j->rax_at = -1;
            vpop(j);
            e8(j, 0x48); e8(j, 0x63); e8(j, 0xC0);              // movsxd rax, eax
            vpush(j, V_RAX);
            break;
        case 0xC4: op_m(j, 0, 1, 0, 0x63, RAX, SL(1)); st64(j, RAX, SL(1)); break;   // movsxd
        case 0xC0: op_m(j, 0, 0, 1, 0xBE, RAX, SL(1)); st64(j, RAX, SL(1)); break;
        case 0xC1: op_m(j, 0, 0, 1, 0xBF, RAX, SL(1)); st64(j, RAX, SL(1)); break;
        case 0xC2: op_m(j, 0, 1, 1, 0xBE, RAX, SL(1)); st64(j, RAX, SL(1)); break;
        case 0xC3: op_m(j, 0, 1, 1, 0xBF, RAX, SL(1)); st64(j, RAX, SL(1)); break;
        case 0xB2: op_m(j, 0xF3, 0, 1, 0x2A, 0, SL(1)); st_f32(j, 1); break;         // cvtsi2ss из i32
        case 0xB3: ld32(j, RAX, SL(1)); op_r(j, 0xF3, 1, 1, 0x2A, 0, RAX); st_f32(j, 1); break;   // u32: через 64 бита — точно
        case 0xB4: op_m(j, 0xF3, 1, 1, 0x2A, 0, SL(1)); st_f32(j, 1); break;
        case 0xB6: op_m(j, 0xF2, 0, 1, 0x5A, 0, SL(1)); st_f32(j, 1); break;         // cvtsd2ss
        case 0xB7: op_m(j, 0xF2, 0, 1, 0x2A, 0, SL(1)); op_m(j, 0xF2, 0, 1, 0x11, 0, SL(1)); break;
        case 0xB8: ld32(j, RAX, SL(1)); op_r(j, 0xF2, 1, 1, 0x2A, 0, RAX); op_m(j, 0xF2, 0, 1, 0x11, 0, SL(1)); break;
        case 0xB9: op_m(j, 0xF2, 1, 1, 0x2A, 0, SL(1)); op_m(j, 0xF2, 0, 1, 0x11, 0, SL(1)); break;
        case 0xBB: op_m(j, 0xF3, 0, 1, 0x5A, 0, SL(1)); op_m(j, 0xF2, 0, 1, 0x11, 0, SL(1)); break;   // cvtss2sd
        case 0xBC: case 0xBE: ld32(j, RAX, SL(1)); st64(j, RAX, SL(1)); break;      // reinterpret 32: те же биты
        case 0xBD: case 0xBF: break;                                                // reinterpret 64

        default:
            // Всё остальное — редкое или с тонкой семантикой — исполняет
            // интерпретатор, по одной команде (см. шапку файла).
            fallback(j, i, next);
            break;
        }
        i = next;
    }
    j->lab[len] = j->n;

    // ---- общий хвост возврата: результаты уже лежат с начала кадра ----
    uint32_t tail = j->n;
    op_m(j, 0, 1, 0, 0x8D, R13, RBX, 8 * (int32_t)fn->n_results);
    ld64(j, RCX, R14, CTX(ftop));
    op_m(j, 0, 1, 0, 0x3B, RCX, R14, CTX(fbase));
    jcc_exit(j, CC_E, rec_new(j, X_DONE, 0, 0));                // вернулась функция, с которой начинали
    ld64(j, RAX, RCX, (int32_t)offsetof(wasm_frame_t, native) - (int32_t)sizeof(wasm_frame_t));
    op_r(j, 0, 1, 0, 0x85, RAX, RAX);
    jcc_exit(j, CC_E, rec_new(j, X_RETFRAME, 0, 0));            // кадр положил интерпретатор — разберёт водитель
    e8(j, 0x48); e8(j, 0x83); e8(j, 0xE9); e8(j, (uint8_t)sizeof(wasm_frame_t));    // sub rcx, размер кадра
    st64(j, RCX, R14, CTX(ftop));
    ld64(j, RBX, RCX, (int32_t)offsetof(wasm_frame_t, fp));
    e8(j, 0xFF); e8(j, 0xE0);                                   // jmp rax
    for (uint32_t k = 0; k < j->n_rfix; k++) to_off(j, j->rfix[k], tail);

    // ---- выходы: ловушки (по одной на вид) и записи ----
    for (int t = 1; t <= WASM_TRAP_NOMEM && !j->fail; t++) {
        uint32_t at = 0;
        for (uint32_t k = 0; k < j->n_stub; k++) {
            if (j->stub[k].rec || j->stub[k].trap != t) continue;
            if (!at) {
                at = j->n;
                mov_ptr(j, RAX, rec_new(j, X_TRAP, t, 0));
                to_off(j, jmp_fwd(j), j->exit_common);
            }
            to_off(j, j->stub[k].pos, at);
        }
    }
    for (uint32_t k = 0; k < j->n_stub && !j->fail; k++) {
        stub_t* st = &j->stub[k];
        if (!st->rec) continue;
        to_off(j, st->pos, j->n);
        if (st->has_aux) { mov_ptr(j, RAX, st->aux); st64(j, RAX, R14, CTX(aux)); }
        mov_ptr(j, RAX, st->rec);
        to_off(j, jmp_fwd(j), j->exit_common);
    }
    // ---- переходы внутри функции ----
    for (uint32_t k = 0; k < j->n_fix && !j->fail; k++) {
        uint32_t t = j->fix[k].target;
        if (t > len || j->lab[t] == 0xFFFFFFFFu) { j->fail = true; break; }     // такого проверка не пишет
        to_off(j, j->fix[k].pos, j->lab[t]);
    }
    wasm_port_free(j->lab); wasm_port_free(j->tgt);
    j->lab = 0; j->tgt = 0;
}

// ---------------------------------------------------------------------------
// Модуль: все его функции — в один кусок исполняемой памяти.
// ---------------------------------------------------------------------------
static bool g_jit_on = true;
void wasm_jit_default(bool on) { g_jit_on = on; }
bool wasm_jit_enabled(void) { return g_jit_on; }

bool wasm_jit_compile(wasm_store_t* s, wasm_instance_t* inst) {
    const wasm_module_t* m = inst->module;
    uint32_t n_own = m->n_funcs - m->n_imp_funcs;
    if (!s->jit || s->jit_off) return false;
    if (!n_own) return true;
    J jj;
    J* j = &jj;
    for (size_t i = 0; i < sizeof(jj); i++) ((uint8_t*)j)[i] = 0;
    j->s = s; j->inst = inst;

    // Вход из C: enter(ctx, адрес) — сохранить регистры, взять свои из
    // контекста, прыгнуть. И общий хвост выхода: всё обратно, в rax — запись.
    static const uint8_t pre[] = {
        0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,     // push rbx, rbp, r12..r15
        0x49, 0x89, 0xFE,                                               // mov r14, rdi
    };
    for (size_t i = 0; i < sizeof(pre); i++) e8(j, pre[i]);
    ld64(j, RBX, R14, CTX(fp));
    ld64(j, R13, R14, CTX(sp));
    ld64(j, R15, R14, CTX(ticks));
    ld64(j, R12, R14, CTX(mem));
    ld64(j, RBP, R14, CTX(mem_bytes));
    e8(j, 0xFF); e8(j, 0xE6);                                           // jmp rsi
    j->exit_common = j->n;
    st64(j, RBX, R14, CTX(fp));
    st64(j, R13, R14, CTX(sp));
    st64(j, R15, R14, CTX(ticks));
    static const uint8_t post[] = { 0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C, 0x5D, 0x5B, 0xC3 };
    for (size_t i = 0; i < sizeof(post); i++) e8(j, post[i]);

    uint32_t* entry = (uint32_t*)wasm_port_alloc((size_t)n_own * sizeof(uint32_t));
    wasm_jit_pt_t** maps = (wasm_jit_pt_t**)wasm_port_alloc((size_t)n_own * sizeof(void*));
    if (!entry || !maps) j->fail = true;
    for (uint32_t k = 0; k < n_own && !j->fail; k++) {
        const wasm_func_t* d = &m->funcs[m->n_imp_funcs + k];
        wasm_funcinst_t* f = inst->funcs[m->n_imp_funcs + k];
        compile_func(j, f, d->ir, d->ir_len, &entry[k]);
        if (j->fail) break;
        // Точки входа функции — в память хранилища (пока со смещениями).
        wasm_jit_pt_t* mp = (wasm_jit_pt_t*)wasm_blk_alloc(&s->arena, (uint64_t)(j->n_map ? j->n_map : 1) * sizeof(*mp), 0);
        if (!mp) { j->fail = true; break; }
        for (uint32_t q = 0; q < j->n_map; q++) mp[q] = j->map[q];
        maps[k] = mp;
        f->jit_map = mp; f->jit_n = j->n_map;
    }
    uint8_t* x = 0;
    if (!j->fail) {
        for (uint32_t k = 0; k < j->n_ffix; k++) to_off(j, j->ffix[k].pos, entry[j->ffix[k].func]);
        x = (uint8_t*)wasm_port_exec_alloc(j->n);
        if (!x) j->fail = true;
    }
    if (!j->fail) {
        for (uint32_t i = 0; i < j->n; i++) x[i] = j->p[i];
        for (jit_exit_t* r = j->recs; r; r = r->next) r->cont = x + r->cont_off;
        for (uint32_t k = 0; k < n_own; k++) {
            wasm_funcinst_t* f = inst->funcs[m->n_imp_funcs + k];
            for (uint32_t q = 0; q < f->jit_n; q++) maps[k][q].native = x + (uint32_t)(uintptr_t)maps[k][q].native;
            f->jit = x + entry[k];
        }
        wasm_jit_block_t* blk = (wasm_jit_block_t*)wasm_blk_alloc(&s->arena, sizeof(*blk), 0);
        if (blk) { blk->code = x; blk->bytes = j->n; blk->next = s->jit_blocks; s->jit_blocks = blk; }
        if (!s->jit_enter) s->jit_enter = x;                    // вход из C одинаков у всех кусков
        s->jit_bytes += j->n;
        s->jit_funcs += n_own;
    } else {
        // Не вышло (нет памяти) — хранилище целиком остаётся на интерпретаторе:
        // смешивать функции с машинным кодом и без него водитель не умеет.
        for (uint32_t k = 0; k < n_own; k++) { wasm_funcinst_t* f = inst->funcs[m->n_imp_funcs + k]; f->jit = 0; f->jit_map = 0; f->jit_n = 0; }
        s->jit_off = true;
    }
    if (entry) wasm_port_free(entry);
    if (maps) wasm_port_free(maps);
    if (j->p) wasm_port_free(j->p);
    if (j->fix) wasm_port_free(j->fix);
    if (j->stub) wasm_port_free(j->stub);
    if (j->rfix) wasm_port_free(j->rfix);
    if (j->map) wasm_port_free(j->map);
    if (j->ffix) wasm_port_free(j->ffix);
    if (j->lab) wasm_port_free(j->lab);
    if (j->tgt) wasm_port_free(j->tgt);
    return !j->fail;
}

void wasm_jit_free(wasm_store_t* s) {
    for (wasm_jit_block_t* b = s->jit_blocks; b; b = b->next) wasm_port_exec_free(b->code, b->bytes);
    s->jit_blocks = 0;
}

// ---------------------------------------------------------------------------
// Водитель: то же, что wasm_interp, но счёт идёт в машинном коде.
// ---------------------------------------------------------------------------
// Место во внутреннем коде -> адрес в машинном. Точек входа немного (начало
// функции, циклы, возвраты из вызовов, места после «чужих» команд), они идут
// по возрастанию — двоичный поиск.
static void* lookup(const wasm_funcinst_t* f, const uint64_t* ip) {
    uint32_t lo = 0, hi = f->jit_n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (f->jit_map[mid].ip < ip) lo = mid + 1; else hi = mid;
    }
    return (lo < f->jit_n && f->jit_map[lo].ip == ip) ? f->jit_map[lo].native : 0;
}

typedef const jit_exit_t* (*jit_enter_fn)(wasm_jit_ctx_t* ctx, void* native);

int wasm_jit_run(wasm_store_t* s, wasm_funcinst_t* fn) {
    wasm_jit_ctx_t* c = &s->jc;
    jit_enter_fn enter = (jit_enter_fn)(uintptr_t)s->jit_enter;
    const int unit = (int)s->tick_unit;
    wasm_funcinst_t* cur;
    void* native;
    int ticks;
    bool step = false;

    c->fbase = s->frames;
    c->fend = s->frames + s->cfg.max_frames;
    c->stack_end = s->stack + s->cfg.stack_slots;
    if (fn) {
        cur = fn;
        c->fp = s->stack; c->sp = s->stack;
        c->ftop = s->frames;
        ticks = unit;
        native = fn->jit;
    } else {
        // Продолжение после паузы: положение — из хранилища, в том же виде, в
        // каком его оставил бы интерпретатор.
        cur = s->r_fn;
        c->fp = s->r_fp; c->sp = s->r_sp;
        c->ftop = s->frames + s->r_depth;
        ticks = s->r_ticks;
        native = 0;
        // Встали на пороге вызова — сам вызов сделает интерпретатор (один шаг).
        if (s->r_pend) step = true;
        else native = lookup(cur, s->r_ip);
        if (!step && !native) return WASM_TRAP_HOST;
    }
    for (;;) {
        if (step) {
            // Одна команда — интерпретатору. Положение у нас с ним общее.
            step = false;
            s->r_fp = c->fp; s->r_sp = c->sp;
            s->r_depth = (uint32_t)(c->ftop - s->frames);
            s->r_ticks = ticks;
            s->step_once = true;
            int rc = wasm_interp(s, 0);
            s->step_once = false;
            if (rc != WASM_STEPPED) return rc;      // ловушка, конец или пауза (положение уже сохранено)
            cur = s->r_fn;
            c->fp = s->r_fp; c->sp = s->r_sp;
            c->ftop = s->frames + s->r_depth;
            ticks = s->r_ticks;
            native = lookup(cur, s->r_ip);
            if (!native) return WASM_TRAP_HOST;     // сюда попасть нельзя: ошибка движка
        }
        if (cur->inst->mem) { c->mem = cur->inst->mem->data; c->mem_bytes = cur->inst->mem->bytes; }
        else { c->mem = 0; c->mem_bytes = 0; }
        c->ticks = (uint64_t)(uint32_t)ticks;
        const jit_exit_t* r = enter(c, native);
        ticks = (int)(uint32_t)c->ticks;
        switch (r->kind) {
        case X_DONE: return WASM_TRAP_NONE;
        case X_TRAP: return r->trap;
        case X_TICK: case X_TICK_CALL:
            // То же, что TICK() в интерпретаторе.
            ticks = unit; s->ticks++;
            if (s->stop && *s->stop) return WASM_TRAP_STOPPED;
            if (s->fuel >= 0 && --s->fuel < 0) { s->fuel = 0; return WASM_TRAP_FUEL; }
            if (s->slice_left > 0 && --s->slice_left == 0) {
                s->r_fn = r->fn; s->r_fp = c->fp; s->r_sp = c->sp; s->r_ip = r->ip;
                s->r_depth = (uint32_t)(c->ftop - s->frames); s->r_ticks = ticks;
                s->r_pend = r->kind == X_TICK_CALL ? (wasm_funcinst_t*)(uintptr_t)c->aux : 0;
                return WASM_PAUSED_YIELD;
            }
            cur = r->fn; native = r->cont;
            break;
        case X_CALLOUT:
            s->r_fn = r->fn; s->r_ip = r->ip;
            s->r_pend = (wasm_funcinst_t*)(uintptr_t)c->aux;
            step = true;
            break;
        case X_FALLBACK:
            s->r_fn = r->fn; s->r_ip = r->ip; s->r_pend = 0;
            step = true;
            break;
        case X_RETFRAME: {
            // Возврат в кадр, который положил интерпретатор (вызов после паузы):
            // адреса в машинном коде в нём нет — находим по месту во внутреннем.
            wasm_frame_t* fr = --c->ftop;
            cur = fr->fn; c->fp = fr->fp;
            native = lookup(cur, fr->ip);
            if (!native) return WASM_TRAP_HOST;
            break;
        }
        default: return WASM_TRAP_HOST;
        }
    }
}

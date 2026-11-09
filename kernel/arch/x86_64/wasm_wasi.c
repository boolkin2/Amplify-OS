// ============================================================================
// wasm_wasi.c — окружение WASI preview1 (этап 51.2). См. wasm_wasi.h.
// ----------------------------------------------------------------------------
// Каждая функция WASI получает от программы числа: описатели, смещения в её
// памяти, длины. Общее правило этого файла — НЕ ВЕРИТЬ ни одному из них:
//   * смещение + длина сверяются с размером памяти программы (mem_at), иначе
//     EFAULT — чтение и запись идут только внутри её памяти;
//   * описатель — индекс в нашей таблице, сверяется с её размером;
//   * путь разбирается здесь (resolve) и не может выйти из открытого каталога.
// Результат функции — код ошибки WASI в vals[0]; сама программа при этом
// продолжается. Остановить её может только proc_exit и прерванное ожидание.
//
// Расположение структур в памяти программы (fdstat, filestat, dirent, iovec,
// prestat, subscription, event) взято из описания preview1; числа пишутся
// побайтно, младшим вперёд, — выравнивание указателей программы нам не указ.
// ============================================================================
#include "wasm_wasi.h"

enum { FD_FREE = 0, FD_STDIN, FD_STDOUT, FD_STDERR, FD_DIR, FD_FILE };

typedef struct {
    uint8_t  kind;                      // FD_*
    bool     is_preopen, readable, writable, append, dirty;
    uint8_t  pre;                       // в каком заранее открытом каталоге лежит
    char     path[WASI_PATH_MAX];       // путь хозяина
    uint8_t* data;                      // копия файла; байты от size до cap — нули
    uint64_t size, cap, pos;
} wfd_t;

#define N_FUNCS 46

struct wasi {
    wasm_store_t*    store;
    wasi_config_t    cfg;
    uint64_t         max_file, max_total, file_bytes;
    uint64_t         start_ns;
    wfd_t            fds[WASI_MAX_FDS];
    wasm_funcinst_t* funcs[N_FUNCS];
    int              exit_code;
    bool             exited;
    uint64_t         calls;
    wasm_instance_t* inst;              // экземпляр программы (после wasi_prepare)
    uint64_t         wake_ns;           // [51.3] срок сна, не 0 — программа спит (nonblocking)
};

// Права описателя (rights) — программе их показываем, сами по ним не судим:
// действует режим открытия файла и граница каталога.
#define R_FD_READ   (1ull << 1)
#define R_FD_WRITE  (1ull << 6)
#define R_ALL       0x3FFFFFFFull
#define R_FILE      0x08E001FFull       // всё, что относится к открытому файлу
#define R_TTY_IN    0x0820000Aull       // чтение, без перемещения
#define R_TTY_OUT   0x08200048ull       // запись, без перемещения: по этому libc узнаёт терминал

enum { FT_CHAR = 2, FT_DIR = 3, FT_FILE = 4 };
enum { O_CREAT = 1, O_DIRECTORY = 2, O_EXCL = 4, O_TRUNC = 8 };

// ---- мелочи без libc ----------------------------------------------------------------
static uint32_t s_len(const char* s) { uint32_t n = 0; while (s[n]) n++; return n; }
static bool s_eq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void b_copy(uint8_t* d, const uint8_t* s, uint64_t n) { for (uint64_t i = 0; i < n; i++) d[i] = s[i]; }
static void b_zero(uint8_t* d, uint64_t n) { for (uint64_t i = 0; i < n; i++) d[i] = 0; }

static uint32_t get16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t* p) { return get16(p) | (get16(p + 2) << 16); }
static uint64_t get64(const uint8_t* p) { return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32); }
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put64(uint8_t* p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }

// Номер файла для программы: свёртка пути (у FAT своих номеров нет). Не ноль —
// ноль libc понимает как «номер неизвестен» и переспрашивает.
static uint64_t ino_of(const char* path, const char* name) {
    uint64_t h = 1469598103934665603ull;
    for (; *path; path++) h = (h ^ (uint8_t)*path) * 1099511628211ull;
    if (name) { h = (h ^ '/') * 1099511628211ull; for (; *name; name++) h = (h ^ (uint8_t)*name) * 1099511628211ull; }
    return h | 1;
}

// Кусок памяти программы [ptr, ptr + len) или 0, если он в неё не помещается.
static uint8_t* mem_at(wasm_instance_t* caller, uint64_t ptr, uint64_t len) {
    wasm_memory_t* m = caller ? wasm_instance_memory(caller) : 0;
    if (!m) return 0;
    uint64_t bytes = 0;
    uint8_t* d = wasm_memory_data(m, &bytes);
    if (!d || ptr > bytes || len > bytes - ptr) return 0;
    return d + ptr;
}

#define W        wasi_t* w = (wasi_t*)user; w->calls++
#define H        (&w->cfg.host)
#define RET(e)   do { v[0] = (uint32_t)(e); return WASM_TRAP_NONE; } while (0)
#define A32(i)   ((uint32_t)v[i])
#define A64(i)   (v[i])

// ---- таблица описателей -------------------------------------------------------------
static wfd_t* fd_get(wasi_t* w, uint32_t fd) {
    return (fd < WASI_MAX_FDS && w->fds[fd].kind != FD_FREE) ? &w->fds[fd] : 0;
}
static int fd_free_slot(const wasi_t* w) {
    for (int i = 3; i < WASI_MAX_FDS; i++) if (w->fds[i].kind == FD_FREE) return i;
    return -1;
}

static int file_flush(wasi_t* w, wfd_t* f) {
    if (f->kind != FD_FILE || !f->dirty) return WASI_OK;
    static const uint8_t none = 0;
    int e = H->save(H->ctx, f->path, f->data ? f->data : &none, f->size);
    if (e == WASI_OK) f->dirty = false;
    return e;
}

static void file_drop(wasi_t* w, wfd_t* f) {
    if (f->data) { wasm_port_mem_free(f->data, f->cap); w->file_bytes -= f->cap; }
    f->data = 0; f->cap = 0; f->size = 0;
    f->kind = FD_FREE;
}

// Закрыть: сохранить, если меняли, и освободить в любом случае (иначе после
// отказа диска программа не смогла бы освободить описатель вовсе).
static int fd_close_one(wasi_t* w, wfd_t* f) {
    int e = file_flush(w, f);
    file_drop(w, f);
    return e;
}

// В копии файла должно поместиться need байт.
static int file_reserve(wasi_t* w, wfd_t* f, uint64_t need) {
    if (need <= f->cap) return WASI_OK;
    if (need > w->max_file) return WASI_EFBIG;
    // С запасом (вдвое), чтобы запись по килобайту не копировала файл каждый раз.
    uint64_t cap = f->cap ? f->cap * 2 : 16384;
    if (cap < need) cap = need;
    if (cap > w->max_file) cap = w->max_file;
    cap = (cap + 4095) & ~4095ull;
    if (w->file_bytes - f->cap + cap > w->max_total) {
        cap = (need + 4095) & ~4095ull;
        if (w->file_bytes - f->cap + cap > w->max_total) return WASI_ENOSPC;
    }
    uint8_t* nd = (uint8_t*)wasm_port_mem_alloc(cap);       // обнулённый
    if (!nd) return WASI_ENOSPC;
    if (f->size) b_copy(nd, f->data, f->size);
    if (f->data) wasm_port_mem_free(f->data, f->cap);
    w->file_bytes += cap - f->cap;
    f->data = nd;
    f->cap = cap;
    return WASI_OK;
}

static int file_write(wasi_t* w, wfd_t* f, uint64_t pos, const uint8_t* src, uint32_t n) {
    if (pos > w->max_file || pos + n > w->max_file) return WASI_EFBIG;
    int e = file_reserve(w, f, pos + n);
    if (e) return e;
    b_copy(f->data + pos, src, n);
    if (pos + n > f->size) f->size = pos + n;       // дыра между старым концом и pos — уже нули
    f->dirty = true;
    return WASI_OK;
}

static int file_resize(wasi_t* w, wfd_t* f, uint64_t size) {
    if (size > f->size) { int e = file_reserve(w, f, size); if (e) return e; }
    else b_zero(f->data + size, f->size - size);    // хвост обязан остаться нулевым
    f->size = size;
    f->dirty = true;
    return WASI_OK;
}

// Тот же файл уже открыт и изменён — записать его, прежде чем кто-то прочтёт
// файл с диска (второе открытие, stat, переименование).
static void flush_path(wasi_t* w, const char* path) {
    for (int i = 0; i < WASI_MAX_FDS; i++)
        if (w->fds[i].kind == FD_FILE && w->fds[i].dirty && s_eq(w->fds[i].path, path)) (void)file_flush(w, &w->fds[i]);
}

// ---- граница: путь программы -> путь хозяина ---------------------------------------
// Путь всегда задаётся относительно открытого каталога dirfd. Собираем его по
// компонентам поверх пути этого каталога; «..» снимает компонент, но не глубже
// корня заранее открытого каталога — иначе ENOTCAPABLE. Ссылок в FAT нет, так
// что другого способа выйти наружу не существует.
static int resolve(wasi_t* w, wasm_instance_t* caller, uint32_t dirfd, uint32_t ptr, uint32_t len,
                   char* out, uint8_t* pre_out, bool* is_root) {
    wfd_t* d = fd_get(w, dirfd);
    if (!d) return WASI_EBADF;
    if (d->kind != FD_DIR) return WASI_ENOTDIR;
    const uint8_t* p = mem_at(caller, ptr, len);
    if (!p) return WASI_EFAULT;
    if (len == 0) return WASI_ENOENT;
    if (len >= WASI_PATH_MAX) return WASI_ENAMETOOLONG;
    if (p[0] == '/') return WASI_EPERM;             // абсолютных путей у программы нет
    uint32_t root_n = s_len(w->cfg.preopens[d->pre].host);
    uint32_t n = 0;
    for (; d->path[n]; n++) out[n] = d->path[n];
    for (uint32_t i = 0; i < len;) {
        uint32_t a = i;
        while (i < len && p[i] != '/') i++;
        uint32_t cl = i - a;
        if (i < len) i++;                           // сам '/'
        if (cl == 0 || (cl == 1 && p[a] == '.')) continue;
        if (cl == 2 && p[a] == '.' && p[a + 1] == '.') {
            if (n <= root_n) return WASI_ENOTCAPABLE;
            while (n > root_n && out[n - 1] != '/') n--;
            if (n > root_n) n--;
            continue;
        }
        for (uint32_t k = 0; k < cl; k++) if (p[a + k] < 0x20 || p[a + k] == 0x7F || p[a + k] == '\\') return WASI_EILSEQ;
        if (n + 1 + cl >= WASI_PATH_MAX) return WASI_ENAMETOOLONG;
        out[n++] = '/';
        for (uint32_t k = 0; k < cl; k++) out[n++] = (char)p[a + k];
    }
    out[n] = 0;
    *pre_out = d->pre;
    if (is_root) *is_root = n == root_n;
    return WASI_OK;
}

// Пути нет. По POSIX причина бывает двоякой: нет имени (ENOENT) или на пути
// стоит файл вместо каталога (ENOTDIR) — программе это разные ошибки.
static int missing_errno(wasi_t* w, const char* path, uint8_t pre) {
    char parent[WASI_PATH_MAX];
    uint32_t root_n = s_len(w->cfg.preopens[pre].host);
    uint32_t n = s_len(path);
    for (uint32_t i = 0; i < n; i++) parent[i] = path[i];
    // Поднимаемся к первому существующему предку: он и решает.
    for (;;) {
        while (n > root_n && parent[n - 1] != '/') n--;
        if (n <= root_n) return WASI_ENOENT;
        parent[--n] = 0;
        wasi_stat_t st;
        if (H->stat(H->ctx, parent, &st) == WASI_OK) return st.is_dir ? WASI_ENOENT : WASI_ENOTDIR;
    }
}

static int stat_path(wasi_t* w, const char* path, uint8_t pre, wasi_stat_t* st) {
    int e = H->stat(H->ctx, path, st);
    if (e == WASI_ENOENT) return missing_errno(w, path, pre);
    return e;
}

static void put_filestat(uint8_t* p, uint64_t ino, uint8_t type, uint64_t size, uint64_t mtime) {
    b_zero(p, 64);
    put64(p, 1);                        // устройство
    put64(p + 8, ino);
    p[16] = type;
    put64(p + 24, 1);                   // ссылок
    put64(p + 32, size);
    put64(p + 40, mtime); put64(p + 48, mtime); put64(p + 56, mtime);
}

// ---- аргументы и окружение ----------------------------------------------------------
static int strings_sizes(wasm_instance_t* c, wasm_val_t* v, int n, const char* const* list) {
    uint8_t* pc = mem_at(c, A32(0), 4);
    uint8_t* ps = mem_at(c, A32(1), 4);
    if (!pc || !ps) RET(WASI_EFAULT);
    uint32_t total = 0;
    for (int i = 0; i < n; i++) total += s_len(list[i]) + 1;
    put32(pc, (uint32_t)n);
    put32(ps, total);
    RET(WASI_OK);
}
static int strings_get(wasm_instance_t* c, wasm_val_t* v, int n, const char* const* list) {
    uint64_t pv = A32(0), pb = A32(1);
    for (int i = 0; i < n; i++) {
        uint32_t len = s_len(list[i]) + 1;
        uint8_t* slot = mem_at(c, pv + 4ull * (uint32_t)i, 4);
        uint8_t* dst = mem_at(c, pb, len);
        if (!slot || !dst || pb > 0xFFFFFFFFull) RET(WASI_EFAULT);
        put32(slot, (uint32_t)pb);
        b_copy(dst, (const uint8_t*)list[i], len);
        pb += len;
    }
    RET(WASI_OK);
}
static int w_args_sizes_get(wasm_instance_t* c, wasm_val_t* v, void* user)    { W; return strings_sizes(c, v, w->cfg.argc, w->cfg.argv); }
static int w_args_get(wasm_instance_t* c, wasm_val_t* v, void* user)          { W; return strings_get(c, v, w->cfg.argc, w->cfg.argv); }
static int w_environ_sizes_get(wasm_instance_t* c, wasm_val_t* v, void* user) { W; return strings_sizes(c, v, w->cfg.envc, w->cfg.envp); }
static int w_environ_get(wasm_instance_t* c, wasm_val_t* v, void* user)       { W; return strings_get(c, v, w->cfg.envc, w->cfg.envp); }

// ---- часы, случайные байты, завершение -----------------------------------------------
// Часы: 0 — настенные, 1 — монотонные, 2 и 3 — «время процесса» (у нас — от запуска).
static uint64_t clock_now(wasi_t* w, uint32_t id) {
    if (id == 0) return H->clock_ns(H->ctx, true);
    uint64_t t = H->clock_ns(H->ctx, false);
    return id == 1 ? t : t - w->start_ns;
}
static int w_clock_res_get(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W;
    uint8_t* p = mem_at(c, A32(1), 8);
    if (A32(0) > 3) RET(WASI_EINVAL);
    if (!p) RET(WASI_EFAULT);
    put64(p, 1000000);                  // миллисекунда: точнее таймер ядра не считает
    RET(WASI_OK);
}
static int w_clock_time_get(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W;
    uint8_t* p = mem_at(c, A32(2), 8);
    if (A32(0) > 3) RET(WASI_EINVAL);
    if (!p) RET(WASI_EFAULT);
    put64(p, clock_now(w, A32(0)));
    RET(WASI_OK);
}
static int w_random_get(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W;
    uint8_t* p = mem_at(c, A32(0), A32(1));
    if (!p) RET(WASI_EFAULT);
    for (uint32_t done = 0; done < A32(1);) {
        uint32_t part = A32(1) - done > 4096 ? 4096 : A32(1) - done;
        H->random(H->ctx, p + done, part);
        done += part;
    }
    RET(WASI_OK);
}
static int w_sched_yield(wasm_instance_t* c, wasm_val_t* v, void* user) { W; (void)c; RET(WASI_OK); }
static int w_proc_exit(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; (void)c;
    w->exit_code = (int)A32(0);
    w->exited = true;
    return WASM_TRAP_EXIT;              // не ошибка: так программа и заканчивается
}
static int w_nosys(wasm_instance_t* c, wasm_val_t* v, void* user)  { W; (void)c; RET(WASI_ENOSYS); }
static int w_notsup(wasm_instance_t* c, wasm_val_t* v, void* user) { W; (void)c; RET(WASI_ENOTSUP); }

// ---- чтение и запись ----------------------------------------------------------------
#define MAX_IOVS 1024

// Общая часть fd_write и fd_pwrite. at — где писать в файле; для потоков не важен.
static int do_write(wasi_t* w, wasm_instance_t* c, wasm_val_t* v, wfd_t* f, uint32_t iovs, uint32_t n_iovs,
                    uint32_t p_done, bool positional, uint64_t at) {
    uint8_t* pn = mem_at(c, p_done, 4);
    if (!pn) RET(WASI_EFAULT);
    if (n_iovs > MAX_IOVS) RET(WASI_EINVAL);
    if (f->kind == FD_DIR) RET(WASI_EISDIR);
    if (f->kind == FD_STDIN || (f->kind == FD_FILE && !f->writable)) RET(WASI_EBADF);
    if (positional && f->kind != FD_FILE) RET(WASI_ESPIPE);
    uint64_t total = 0;
    for (uint32_t i = 0; i < n_iovs; i++) {
        const uint8_t* io = mem_at(c, (uint64_t)iovs + 8ull * i, 8);
        if (!io) RET(WASI_EFAULT);
        uint32_t len = get32(io + 4);
        if (!len) continue;
        const uint8_t* src = mem_at(c, get32(io), len);
        if (!src) RET(WASI_EFAULT);
        if (f->kind == FD_FILE) {
            uint64_t pos = positional ? at + total : (f->append ? f->size : f->pos);
            int e = file_write(w, f, pos, src, len);
            if (e) { if (!total) RET(e); break; }
            if (!positional) f->pos = pos + len;
        } else {
            H->out(H->ctx, f->kind == FD_STDERR ? 2 : 1, src, len);
        }
        total += len;
    }
    put32(pn, (uint32_t)total);
    RET(WASI_OK);
}

static int do_read(wasi_t* w, wasm_instance_t* c, wasm_val_t* v, wfd_t* f, uint32_t iovs, uint32_t n_iovs,
                   uint32_t p_done, bool positional, uint64_t at) {
    uint8_t* pn = mem_at(c, p_done, 4);
    if (!pn) RET(WASI_EFAULT);
    if (n_iovs > MAX_IOVS) RET(WASI_EINVAL);
    if (f->kind == FD_DIR) RET(WASI_EISDIR);
    if (f->kind == FD_STDOUT || f->kind == FD_STDERR || (f->kind == FD_FILE && !f->readable)) RET(WASI_EBADF);
    if (positional && f->kind != FD_FILE) RET(WASI_ESPIPE);
    uint64_t total = 0;
    for (uint32_t i = 0; i < n_iovs; i++) {
        const uint8_t* io = mem_at(c, (uint64_t)iovs + 8ull * i, 8);
        if (!io) RET(WASI_EFAULT);
        uint32_t len = get32(io + 4), got;
        if (!len) continue;
        uint8_t* dst = mem_at(c, get32(io), len);
        if (!dst) RET(WASI_EFAULT);
        if (f->kind == FD_FILE) {
            uint64_t pos = positional ? at + total : f->pos;
            uint64_t left = pos < f->size ? f->size - pos : 0;
            got = left < len ? (uint32_t)left : len;
            if (got) b_copy(dst, f->data + pos, got);
            if (!positional) f->pos = pos + got;
        } else {
            got = H->in ? H->in(H->ctx, dst, len) : 0;
            if (got == WASI_IN_WAIT) {
                // Ввода пока нет. Если уже что-то прочли — отдаём это; иначе
                // вызов повторят позже (до этого места мы ничего не изменили).
                if (total) break;
                return WASM_HOST_RETRY;
            }
            if (got > len) got = len;
        }
        total += got;
        if (got < len) break;           // данных меньше, чем просили: дальше не читаем
    }
    put32(pn, (uint32_t)total);
    RET(WASI_OK);
}

static int w_fd_write(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; wfd_t* f = fd_get(w, A32(0));
    if (!f) RET(WASI_EBADF);
    return do_write(w, c, v, f, A32(1), A32(2), A32(3), false, 0);
}
static int w_fd_pwrite(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; wfd_t* f = fd_get(w, A32(0));
    if (!f) RET(WASI_EBADF);
    return do_write(w, c, v, f, A32(1), A32(2), A32(4), true, A64(3));
}
static int w_fd_read(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; wfd_t* f = fd_get(w, A32(0));
    if (!f) RET(WASI_EBADF);
    return do_read(w, c, v, f, A32(1), A32(2), A32(3), false, 0);
}
static int w_fd_pread(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; wfd_t* f = fd_get(w, A32(0));
    if (!f) RET(WASI_EBADF);
    return do_read(w, c, v, f, A32(1), A32(2), A32(4), true, A64(3));
}

static int w_fd_seek(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; wfd_t* f = fd_get(w, A32(0));
    uint8_t* p = mem_at(c, A32(3), 8);
    if (!f) RET(WASI_EBADF);
    if (f->kind != FD_FILE) RET(f->kind == FD_DIR ? WASI_EBADF : WASI_ESPIPE);
    if (!p) RET(WASI_EFAULT);
    uint64_t base = A32(2) == 0 ? 0 : A32(2) == 1 ? f->pos : A32(2) == 2 ? f->size : ~0ull;
    int64_t off = (int64_t)A64(1);
    if (base == ~0ull) RET(WASI_EINVAL);
    // Итог обязан быть неотрицательным и не «обернуться» за 63 бита.
    if (off < 0 ? (uint64_t)(-(off + 1)) + 1 > base : (uint64_t)off > 0x7FFFFFFFFFFFFFFFull - base) RET(WASI_EINVAL);
    f->pos = base + (uint64_t)off;
    put64(p, f->pos);
    RET(WASI_OK);
}
static int w_fd_tell(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; wfd_t* f = fd_get(w, A32(0));
    uint8_t* p = mem_at(c, A32(1), 8);
    if (!f) RET(WASI_EBADF);
    if (f->kind != FD_FILE) RET(WASI_ESPIPE);
    if (!p) RET(WASI_EFAULT);
    put64(p, f->pos);
    RET(WASI_OK);
}

static int w_fd_close(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; (void)c; wfd_t* f = fd_get(w, A32(0));
    if (!f) RET(WASI_EBADF);
    RET(fd_close_one(w, f));
}
static int w_fd_sync(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; (void)c; wfd_t* f = fd_get(w, A32(0));
    if (!f) RET(WASI_EBADF);
    RET(file_flush(w, f));
}
static int w_fd_advise(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; (void)c;
    RET(fd_get(w, A32(0)) ? WASI_OK : WASI_EBADF);  // подсказка о порядке чтения: нам нечего с ней делать
}
static int w_fd_allocate(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; (void)c; wfd_t* f = fd_get(w, A32(0));
    if (!f || f->kind != FD_FILE || !f->writable) RET(WASI_EBADF);
    uint64_t end = A64(1) + A64(2);
    if (end < A64(1) || end > w->max_file) RET(WASI_EFBIG);
    RET(end > f->size ? file_resize(w, f, end) : WASI_OK);
}
static int w_fd_filestat_set_size(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; (void)c; wfd_t* f = fd_get(w, A32(0));
    if (!f || f->kind != FD_FILE || !f->writable) RET(WASI_EBADF);
    if (A64(1) > w->max_file) RET(WASI_EFBIG);
    RET(file_resize(w, f, A64(1)));
}
static int w_fd_fdstat_set_flags(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; (void)c; wfd_t* f = fd_get(w, A32(0));
    if (!f) RET(WASI_EBADF);
    if (f->kind == FD_FILE) f->append = (A32(1) & 1) != 0;
    RET(WASI_OK);
}
static int w_fd_renumber(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; (void)c;
    wfd_t* from = fd_get(w, A32(0));
    if (!from || A32(1) >= WASI_MAX_FDS) RET(WASI_EBADF);
    wfd_t* to = &w->fds[A32(1)];
    if (from == to) RET(WASI_OK);
    if (to->kind != FD_FREE) (void)fd_close_one(w, to);
    *to = *from;
    from->kind = FD_FREE; from->data = 0; from->cap = 0;
    RET(WASI_OK);
}

static int w_fd_fdstat_get(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; wfd_t* f = fd_get(w, A32(0));
    uint8_t* p = mem_at(c, A32(1), 24);
    if (!f) RET(WASI_EBADF);
    if (!p) RET(WASI_EFAULT);
    b_zero(p, 24);
    uint64_t base, inherit = 0;
    switch (f->kind) {
    case FD_STDIN:  p[0] = FT_CHAR; base = R_TTY_IN; break;
    case FD_STDOUT:
    case FD_STDERR: p[0] = FT_CHAR; base = R_TTY_OUT; break;
    case FD_DIR:    p[0] = FT_DIR;  base = R_ALL; inherit = R_ALL; break;
    default:
        p[0] = FT_FILE;
        base = R_FILE & ~((f->readable ? 0 : R_FD_READ) | (f->writable ? 0 : R_FD_WRITE));
        if (f->append) p[2] = 1;
        break;
    }
    put64(p + 8, base);
    put64(p + 16, inherit);
    RET(WASI_OK);
}

static int w_fd_filestat_get(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; wfd_t* f = fd_get(w, A32(0));
    uint8_t* p = mem_at(c, A32(1), 64);
    if (!f) RET(WASI_EBADF);
    if (!p) RET(WASI_EFAULT);
    if (f->kind == FD_FILE) put_filestat(p, ino_of(f->path, 0), FT_FILE, f->size, 0);
    else if (f->kind == FD_DIR) put_filestat(p, ino_of(f->path, 0), FT_DIR, 0, 0);
    else put_filestat(p, A32(0) + 1, FT_CHAR, 0, 0);
    RET(WASI_OK);
}

static int w_fd_prestat_get(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; wfd_t* f = fd_get(w, A32(0));
    uint8_t* p = mem_at(c, A32(1), 8);
    if (!f || !f->is_preopen) RET(WASI_EBADF);      // по этому ответу libc узнаёт, что каталоги кончились
    if (!p) RET(WASI_EFAULT);
    b_zero(p, 8);                                   // вид 0 — каталог
    put32(p + 4, s_len(w->cfg.preopens[f->pre].guest));
    RET(WASI_OK);
}
static int w_fd_prestat_dir_name(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; wfd_t* f = fd_get(w, A32(0));
    if (!f || !f->is_preopen) RET(WASI_EBADF);
    const char* name = w->cfg.preopens[f->pre].guest;
    uint32_t n = s_len(name);
    if (A32(2) < n) RET(WASI_ENAMETOOLONG);
    uint8_t* p = mem_at(c, A32(1), n);
    if (!p) RET(WASI_EFAULT);
    b_copy(p, (const uint8_t*)name, n);
    RET(WASI_OK);
}

// ---- пути ---------------------------------------------------------------------------
static int w_path_open(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W;
    uint32_t oflags = A32(4), fdflags = A32(7);
    uint64_t rights = A64(5);
    uint8_t* pfd = mem_at(c, A32(8), 4);
    char path[WASI_PATH_MAX];
    uint8_t pre;
    if (!pfd) RET(WASI_EFAULT);
    int e = resolve(w, c, A32(0), A32(2), A32(3), path, &pre, 0);
    if (e) RET(e);
    bool ro = w->cfg.preopens[pre].read_only;
    bool want_write = (rights & R_FD_WRITE) != 0 || (oflags & O_TRUNC) != 0;
    int nfd = fd_free_slot(w);
    if (nfd < 0) RET(WASI_EMFILE);

    wasi_stat_t st;
    e = H->stat(H->ctx, path, &st);
    if (e && e != WASI_ENOENT) RET(e);
    if (e == WASI_OK && (oflags & O_CREAT) && (oflags & O_EXCL)) RET(WASI_EEXIST);
    if (e == WASI_ENOENT) {
        if (!(oflags & O_CREAT)) RET(missing_errno(w, path, pre));
        if (oflags & O_DIRECTORY) RET(WASI_EINVAL);
        if (ro) RET(WASI_EROFS);
        // Файл появляется на диске сразу (пустым): его должны видеть stat и
        // второе открытие, а не только мы до закрытия.
        static const uint8_t none = 0;
        e = H->save(H->ctx, path, &none, 0);
        if (e) RET(e == WASI_ENOENT ? missing_errno(w, path, pre) : e);
        st.is_dir = false; st.size = 0;
    }

    wfd_t* f = &w->fds[nfd];
    if (st.is_dir) {
        if (want_write) RET(WASI_EISDIR);
        f->data = 0; f->cap = 0; f->size = 0; f->pos = 0;
        f->is_preopen = false; f->readable = true; f->writable = false; f->append = false; f->dirty = false;
    } else {
        if (oflags & O_DIRECTORY) RET(WASI_ENOTDIR);
        if (want_write && ro) RET(WASI_EROFS);
        flush_path(w, path);
        if (st.size > w->max_file) RET(WASI_EFBIG);
        bool trunc = (oflags & O_TRUNC) != 0;
        f->data = 0; f->cap = 0; f->size = 0; f->pos = 0;
        if (!trunc && st.size) {
            // Копия файла целиком. Повторно спрашиваем размер после flush_path: он мог вырасти.
            if (H->stat(H->ctx, path, &st) != WASI_OK || st.is_dir) RET(WASI_EIO);
            if (st.size > w->max_file) RET(WASI_EFBIG);
            e = file_reserve(w, f, st.size);
            if (e) RET(e);
            e = H->load(H->ctx, path, f->data, st.size);
            if (e) { wasm_port_mem_free(f->data, f->cap); w->file_bytes -= f->cap; f->data = 0; f->cap = 0; RET(e); }
            f->size = st.size;
        }
        f->is_preopen = false;
        f->writable = (rights & R_FD_WRITE) != 0;
        f->readable = (rights & R_FD_READ) != 0 || !f->writable;
        f->append = (fdflags & 1) != 0;
        f->dirty = trunc && st.size != 0;           // обрезка должна дойти до диска и без единой записи
    }
    f->kind = st.is_dir ? FD_DIR : FD_FILE;
    f->pre = pre;
    uint32_t n = 0;
    for (; path[n]; n++) f->path[n] = path[n];
    f->path[n] = 0;
    put32(pfd, (uint32_t)nfd);
    RET(WASI_OK);
}

static int w_path_filestat_get(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W;
    uint8_t* p = mem_at(c, A32(4), 64);
    char path[WASI_PATH_MAX];
    uint8_t pre;
    if (!p) RET(WASI_EFAULT);
    int e = resolve(w, c, A32(0), A32(2), A32(3), path, &pre, 0);
    if (e) RET(e);
    wasi_stat_t st;
    e = stat_path(w, path, pre, &st);
    if (e) RET(e);
    // Файл открыт у самой программы — верен размер её копии, а не того, что на диске.
    for (int i = 0; i < WASI_MAX_FDS; i++)
        if (w->fds[i].kind == FD_FILE && s_eq(w->fds[i].path, path)) { st.size = w->fds[i].size; break; }
    put_filestat(p, ino_of(path, 0), st.is_dir ? FT_DIR : FT_FILE, st.is_dir ? 0 : st.size, st.mtime_ns);
    RET(WASI_OK);
}

static int w_path_create_directory(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W;
    char path[WASI_PATH_MAX];
    uint8_t pre;
    bool is_root;
    int e = resolve(w, c, A32(0), A32(1), A32(2), path, &pre, &is_root);
    if (e) RET(e);
    wasi_stat_t st;
    if (is_root || H->stat(H->ctx, path, &st) == WASI_OK) RET(WASI_EEXIST);
    if (w->cfg.preopens[pre].read_only) RET(WASI_EROFS);
    e = H->mkdir(H->ctx, path);
    RET(e == WASI_ENOENT ? missing_errno(w, path, pre) : e);
}

static int remove_common(wasi_t* w, wasm_instance_t* c, wasm_val_t* v, bool dir) {
    char path[WASI_PATH_MAX];
    uint8_t pre;
    bool is_root;
    int e = resolve(w, c, A32(0), A32(1), A32(2), path, &pre, &is_root);
    if (e) RET(e);
    wasi_stat_t st;
    e = stat_path(w, path, pre, &st);
    if (e) RET(e);
    if (dir && !st.is_dir) RET(WASI_ENOTDIR);
    if (!dir && st.is_dir) RET(WASI_EISDIR);
    if (is_root) RET(WASI_EPERM);                   // сам открытый каталог удалить нельзя
    if (w->cfg.preopens[pre].read_only) RET(WASI_EROFS);
    // Открытую копию удалённого файла при закрытии на диск уже не пишем.
    if (!dir) for (int i = 0; i < WASI_MAX_FDS; i++)
        if (w->fds[i].kind == FD_FILE && s_eq(w->fds[i].path, path)) { w->fds[i].dirty = false; w->fds[i].writable = false; }
    RET(H->remove(H->ctx, path, dir));
}
static int w_path_unlink_file(wasm_instance_t* c, wasm_val_t* v, void* user)      { W; return remove_common(w, c, v, false); }
static int w_path_remove_directory(wasm_instance_t* c, wasm_val_t* v, void* user) { W; return remove_common(w, c, v, true); }

static int w_path_rename(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W;
    char from[WASI_PATH_MAX], to[WASI_PATH_MAX];
    uint8_t pre_a, pre_b;
    bool root_a, root_b;
    int e = resolve(w, c, A32(0), A32(1), A32(2), from, &pre_a, &root_a);
    if (!e) e = resolve(w, c, A32(3), A32(4), A32(5), to, &pre_b, &root_b);
    if (e) RET(e);
    wasi_stat_t sa, sb;
    e = stat_path(w, from, pre_a, &sa);
    if (e) RET(e);
    if (root_a || root_b) RET(WASI_EPERM);
    if (w->cfg.preopens[pre_a].read_only || w->cfg.preopens[pre_b].read_only) RET(WASI_EROFS);
    if (s_eq(from, to)) RET(WASI_OK);
    bool have_b = H->stat(H->ctx, to, &sb) == WASI_OK;
    if (have_b && sb.is_dir != sa.is_dir) RET(sb.is_dir ? WASI_EISDIR : WASI_ENOTDIR);
    flush_path(w, from);
    if (H->rename) e = H->rename(H->ctx, from, to);
    else if (sa.is_dir) e = WASI_ENOTSUP;           // каталог без помощи хозяина не перенести
    else {
        // Хозяин переименовывать не умеет: копия под новым именем, затем
        // удаление старого. Порядок такой, чтобы при сбое файл не пропал.
        if (H->stat(H->ctx, from, &sa) != WASI_OK) RET(WASI_EIO);
        if (sa.size > w->max_file) RET(WASI_EFBIG);
        uint8_t* buf = 0;
        static const uint8_t none = 0;
        if (sa.size) {
            buf = (uint8_t*)wasm_port_mem_alloc(sa.size);
            if (!buf) RET(WASI_ENOSPC);
            e = H->load(H->ctx, from, buf, sa.size);
        }
        if (!e) e = H->save(H->ctx, to, buf ? buf : &none, sa.size);
        if (buf) wasm_port_mem_free(buf, sa.size);
        if (e == WASI_ENOENT) e = missing_errno(w, to, pre_b);
        if (!e) e = H->remove(H->ctx, from, false);
    }
    if (e) RET(e);
    // Открытые описатели файла переезжают вместе с ним.
    for (int i = 0; i < WASI_MAX_FDS; i++)
        if (w->fds[i].kind == FD_FILE && s_eq(w->fds[i].path, from)) {
            uint32_t n = 0;
            for (; to[n]; n++) w->fds[i].path[n] = to[n];
            w->fds[i].path[n] = 0;
            w->fds[i].pre = pre_b;
        }
    RET(WASI_OK);
}

static int w_path_readlink(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W;
    char path[WASI_PATH_MAX];
    uint8_t pre;
    int e = resolve(w, c, A32(0), A32(1), A32(2), path, &pre, 0);
    if (e) RET(e);
    wasi_stat_t st;
    e = stat_path(w, path, pre, &st);
    RET(e ? e : WASI_EINVAL);                       // ссылок нет: всё, что есть, — «не ссылка»
}

// ---- каталог ------------------------------------------------------------------------
typedef struct {
    uint8_t* buf;
    uint32_t cap, used;
    uint64_t cookie, index;
    const char* dir;
} rd_t;

// Запись: 24 байта заголовка + имя. Последняя запись в буфере может быть
// обрезана — тогда буфер заполнен до конца, и libc по этому признаку приходит
// за продолжением с буфером побольше.
static bool rd_entry(void* user, const char* name, bool is_dir) {
    rd_t* r = (rd_t*)user;
    uint64_t idx = r->index++;
    if (idx < r->cookie) return true;
    uint32_t nl = s_len(name);
    uint8_t head[24];
    b_zero(head, 24);
    put64(head, idx + 1);
    put64(head + 8, ino_of(r->dir, name));
    put32(head + 16, nl);
    head[20] = is_dir ? FT_DIR : FT_FILE;
    uint32_t room = r->cap - r->used;
    uint32_t part = room < 24 ? room : 24;
    b_copy(r->buf + r->used, head, part);
    r->used += part; room -= part;
    part = room < nl ? room : nl;
    b_copy(r->buf + r->used, (const uint8_t*)name, part);
    r->used += part;
    return r->used < r->cap;
}

static int w_fd_readdir(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W; wfd_t* f = fd_get(w, A32(0));
    uint8_t* buf = mem_at(c, A32(1), A32(2));
    uint8_t* pn = mem_at(c, A32(4), 4);
    if (!f) RET(WASI_EBADF);
    if (f->kind != FD_DIR) RET(WASI_ENOTDIR);
    if (!buf || !pn) RET(WASI_EFAULT);
    rd_t r = { buf, A32(2), 0, A64(3), 0, f->path };
    if (r.cap) {
        int e = H->list(H->ctx, f->path, rd_entry, &r);
        if (e) RET(e);
    }
    put32(pn, r.used);
    RET(WASI_OK);
}

// ---- ожидание -----------------------------------------------------------------------
// Подписка — 48 байт: метка (8), вид (1 байт по смещению 8), для часов —
// номер часов (16), срок (24), точность (32), признак «срок абсолютный» (40).
// Событие — 32 байта: метка (0), ошибка (8), вид (10).
// Чтение и запись у нас никогда не ждут (файлы — в памяти), поэтому подписка
// на описатель готова сразу; настоящая работа здесь одна — сон до срока.
static int w_poll_oneoff(wasm_instance_t* c, wasm_val_t* v, void* user) {
    W;
    uint32_t n = A32(2);
    if (n == 0 || n > 64) RET(WASI_EINVAL);
    const uint8_t* in = mem_at(c, A32(0), 48ull * n);
    uint8_t* out = mem_at(c, A32(1), 32ull * n);
    uint8_t* pn = mem_at(c, A32(3), 4);
    if (!in || !out || !pn) RET(WASI_EFAULT);
    uint64_t wait[64], min_wait = ~0ull;
    bool any_fd = false;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* s = in + 48ull * i;
        wait[i] = 0;
        if (s[8] != 0) { any_fd = true; continue; }
        uint32_t id = get32(s + 16);
        uint64_t t = get64(s + 24);
        if (id > 3) RET(WASI_EINVAL);
        if (get16(s + 40) & 1) { uint64_t now = clock_now(w, id); t = t > now ? t - now : 0; }
        wait[i] = t;
        if (t < min_wait) min_wait = t;
    }
    if (!any_fd && min_wait) {
        if (w->cfg.nonblocking) {
            // Срок запоминается при первом заходе; пока он не вышел — «повтори».
            uint64_t now = H->clock_ns(H->ctx, false);
            if (!w->wake_ns) w->wake_ns = now + min_wait > now ? now + min_wait : ~0ull;
            if (now < w->wake_ns) return WASM_HOST_RETRY;
            w->wake_ns = 0;
            min_wait = ~0ull;           // сон окончен: срабатывают часы с наименьшим сроком
            for (uint32_t i = 0; i < n; i++) if (in[48ull * i + 8] == 0 && wait[i] < min_wait) min_wait = wait[i];
        } else if (!H->sleep_ns(H->ctx, min_wait)) return WASM_TRAP_STOPPED;
    }
    uint32_t done = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* s = in + 48ull * i;
        bool is_clock = s[8] == 0;
        if (is_clock ? (any_fd ? wait[i] != 0 : wait[i] != min_wait) : false) continue;
        uint8_t* e = out + 32ull * done++;
        b_zero(e, 32);
        b_copy(e, s, 8);
        e[10] = s[8];
    }
    put32(pn, done);
    RET(WASI_OK);
}

// ---- таблица функций ----------------------------------------------------------------
// Типы: i — i32, I — i64. Результат у всех один (код ошибки, i32), кроме
// proc_exit: она не возвращается. Типы обязаны совпасть с импортом модуля
// буква в букву — иначе модуль не свяжется (сверено с wasi-libc узла).
static const struct { const char* name; const char* sig; wasm_host_fn fn; } k_fn[N_FUNCS] = {
    { "args_get", "ii", w_args_get },                       { "args_sizes_get", "ii", w_args_sizes_get },
    { "environ_get", "ii", w_environ_get },                 { "environ_sizes_get", "ii", w_environ_sizes_get },
    { "clock_res_get", "ii", w_clock_res_get },             { "clock_time_get", "iIi", w_clock_time_get },
    { "fd_advise", "iIIi", w_fd_advise },                   { "fd_allocate", "iII", w_fd_allocate },
    { "fd_close", "i", w_fd_close },                        { "fd_datasync", "i", w_fd_sync },
    { "fd_fdstat_get", "ii", w_fd_fdstat_get },             { "fd_fdstat_set_flags", "ii", w_fd_fdstat_set_flags },
    { "fd_fdstat_set_rights", "iII", w_notsup },            { "fd_filestat_get", "ii", w_fd_filestat_get },
    { "fd_filestat_set_size", "iI", w_fd_filestat_set_size }, { "fd_filestat_set_times", "iIIi", w_notsup },
    { "fd_pread", "iiiIi", w_fd_pread },                    { "fd_prestat_get", "ii", w_fd_prestat_get },
    { "fd_prestat_dir_name", "iii", w_fd_prestat_dir_name }, { "fd_pwrite", "iiiIi", w_fd_pwrite },
    { "fd_read", "iiii", w_fd_read },                       { "fd_readdir", "iiiIi", w_fd_readdir },
    { "fd_renumber", "ii", w_fd_renumber },                 { "fd_seek", "iIii", w_fd_seek },
    { "fd_sync", "i", w_fd_sync },                          { "fd_tell", "ii", w_fd_tell },
    { "fd_write", "iiii", w_fd_write },                     { "path_create_directory", "iii", w_path_create_directory },
    { "path_filestat_get", "iiiii", w_path_filestat_get },  { "path_filestat_set_times", "iiiiIIi", w_notsup },
    { "path_link", "iiiiiii", w_notsup },                   { "path_open", "iiiiiIIii", w_path_open },
    { "path_readlink", "iiiiii", w_path_readlink },         { "path_remove_directory", "iii", w_path_remove_directory },
    { "path_rename", "iiiiii", w_path_rename },             { "path_symlink", "iiiii", w_notsup },
    { "path_unlink_file", "iii", w_path_unlink_file },      { "poll_oneoff", "iiii", w_poll_oneoff },
    { "proc_exit", "i", w_proc_exit },                      { "proc_raise", "i", w_nosys },
    { "sched_yield", "", w_sched_yield },                   { "random_get", "ii", w_random_get },
    { "sock_accept", "iii", w_notsup },                     { "sock_recv", "iiiiii", w_notsup },
    { "sock_send", "iiiii", w_notsup },                     { "sock_shutdown", "ii", w_notsup },
};

static bool name_is(const wasm_name_t* n, const char* s) {
    uint32_t i = 0;
    for (; i < n->n && s[i]; i++) if (n->p[i] != (uint8_t)s[i]) return false;
    return i == n->n && !s[i];
}

bool wasi_resolve(void* ctx, const wasm_module_t* m, const wasm_import_t* imp, wasm_extern_t* out) {
    (void)m;
    wasi_t* w = (wasi_t*)ctx;
    if (imp->kind != WK_FUNC || !name_is(&imp->module, "wasi_snapshot_preview1")) return false;
    for (int i = 0; i < N_FUNCS; i++)
        if (name_is(&imp->field, k_fn[i].name)) { out->kind = WK_FUNC; out->ptr = w->funcs[i]; return true; }
    return false;
}

// ---- жизнь окружения ----------------------------------------------------------------
wasi_t* wasi_new(wasm_store_t* s, const wasi_config_t* cfg) {
    if (cfg->n_preopens < 0 || cfg->n_preopens > WASI_MAX_PREOPENS || cfg->argc < 0 || cfg->envc < 0) return 0;
    if (!cfg->host.out || !cfg->host.stat || !cfg->host.load || !cfg->host.save || !cfg->host.mkdir ||
        !cfg->host.remove || !cfg->host.list || !cfg->host.clock_ns || !cfg->host.random ||
        (!cfg->host.sleep_ns && !cfg->nonblocking)) return 0;
    for (int i = 0; i < cfg->n_preopens; i++)
        if (!cfg->preopens[i].guest[0] || s_len(cfg->preopens[i].host) + 2 >= WASI_PATH_MAX) return 0;
    wasi_t* w = (wasi_t*)wasm_port_alloc(sizeof(*w));
    if (!w) return 0;
    b_zero((uint8_t*)w, sizeof(*w));
    w->store = s;
    w->cfg = *cfg;
    w->max_file = cfg->max_file_bytes ? cfg->max_file_bytes : (16ull << 20);
    w->max_total = cfg->max_total_bytes ? cfg->max_total_bytes : (64ull << 20);
    w->start_ns = cfg->host.clock_ns(cfg->host.ctx, false);
    w->fds[0].kind = FD_STDIN;
    w->fds[1].kind = FD_STDOUT;
    w->fds[2].kind = FD_STDERR;
    // Заранее открытые каталоги — подряд с третьего: libc перебирает описатели
    // с 3 и останавливается на первом, про который мы отвечаем «не каталог».
    for (int i = 0; i < cfg->n_preopens; i++) {
        wfd_t* f = &w->fds[3 + i];
        f->kind = FD_DIR; f->is_preopen = true; f->readable = true; f->pre = (uint8_t)i;
        uint32_t n = 0;
        for (; cfg->preopens[i].host[n]; n++) f->path[n] = cfg->preopens[i].host[n];
        f->path[n] = 0;
    }
    for (int i = 0; i < N_FUNCS; i++) {
        uint8_t params[12];
        static const uint8_t res[1] = { WT_I32 };
        uint32_t np = 0;
        for (const char* c = k_fn[i].sig; *c; c++) params[np++] = *c == 'I' ? WT_I64 : WT_I32;
        w->funcs[i] = wasm_host_func(s, params, np, res, k_fn[i].fn == w_proc_exit ? 0 : 1, k_fn[i].fn, w);
        if (!w->funcs[i]) { wasm_port_free(w); return 0; }
    }
    return w;
}

static int close_all(wasi_t* w) {
    int unsaved = 0;
    for (int i = 0; i < WASI_MAX_FDS; i++) {
        if (w->fds[i].kind == FD_FREE) continue;
        if (fd_close_one(w, &w->fds[i]) != WASI_OK) unsaved++;
    }
    return unsaved;
}

int wasi_free(wasi_t* w) {
    if (!w) return 0;
    int unsaved = close_all(w);
    wasm_port_free(w);
    return unsaved;
}

static void text_set(char* dst, uint32_t cap, const char* s) {
    uint32_t n = 0;
    for (; s[n] && n < cap - 1; n++) dst[n] = s[n];
    dst[n] = 0;
}

void wasi_finish(wasm_store_t* s, wasi_t* w, int trap, wasi_result_t* r) {
    (void)s;
    if (trap == WASM_TRAP_NONE) { r->status = WASI_RUN_EXIT; r->exit_code = 0; }
    else if (trap == WASM_TRAP_EXIT && w->exited) { r->status = WASI_RUN_EXIT; r->exit_code = w->exit_code; }
    else { r->status = WASI_RUN_TRAP; r->trap = trap; }
    if (w->inst && wasm_instance_memory(w->inst)) r->memory_pages = wasm_memory_pages(wasm_instance_memory(w->inst));
    // Что программа не закрыла сама (или не успела) — сохраняется здесь.
    r->unsaved = close_all(w);
    r->calls = w->calls;
}

bool wasi_prepare(wasm_store_t* s, const wasm_module_t* m, wasi_t* w, wasi_result_t* r, wasm_funcinst_t** start) {
    b_zero((uint8_t*)r, sizeof(*r));
    *start = 0;
    wasm_inst_err_t ierr;
    w->inst = wasm_instantiate(s, m, wasi_resolve, w, &ierr);
    if (!w->inst) {
        // Модуль с функцией start мог остановиться уже при запуске — это исход
        // программы, а не отказ связывания.
        if (ierr.kind == WASM_INST_TRAP) { wasi_finish(s, w, ierr.trap, r); return false; }
        r->status = WASI_RUN_NOSTART;
        text_set(r->text, sizeof(r->text), ierr.text[0] ? ierr.text : "module did not start");
        r->unsaved = close_all(w);
        return false;
    }
    wasm_extern_t ex;
    const wasm_functype_t* ft = 0;
    if (wasm_instance_export(w->inst, (const uint8_t*)"_start", 6, &ex) && ex.kind == WK_FUNC) ft = wasm_func_type((wasm_funcinst_t*)ex.ptr);
    if (!ft || ft->n_params || ft->n_results) {
        r->status = WASI_RUN_NOSTART;
        text_set(r->text, sizeof(r->text), "no _start function (not a WASI program)");
        r->unsaved = close_all(w);
        return false;
    }
    *start = (wasm_funcinst_t*)ex.ptr;
    return true;
}

void wasi_run(wasm_store_t* s, const wasm_module_t* m, wasi_t* w, wasi_result_t* r) {
    wasm_funcinst_t* start;
    if (!wasi_prepare(s, m, w, r, &start)) return;
    wasi_finish(s, w, wasm_call(s, start, 0, 0), r);
}

uint64_t wasi_wake_ns(const wasi_t* w) { return w->wake_ns; }

// [51.4] Память программы сейчас, страниц по 64 КБ (0 - экземпляра ещё нет или
// у модуля нет памяти). Читает и чужое ядро, пока программа считает: число
// одно, меняется редко, и неточность на один шаг роста окну не вредит.
uint32_t wasi_memory_pages(const wasi_t* w) {
    if (!w || !w->inst || !wasm_instance_memory(w->inst)) return 0;
    return wasm_memory_pages(wasm_instance_memory(w->inst));
}
uint64_t wasi_calls(const wasi_t* w) { return w->calls; }

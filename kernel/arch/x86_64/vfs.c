// ============================================================================
// vfs.c — реализация VFS с поддержкой open-flags, stat, dup, fcntl (этап 40).
// ----------------------------------------------------------------------------
// Основные изменения этапа 40:
//   * fd_entry получает поле kind (FILE/SOCKET/PIPE), refcount и flags.
//   * vfs_open_flags() — понимает O_CREAT/O_TRUNC/O_APPEND/O_EXCL через
//     внешние create/truncate-хуки (fat32_create_file, fat32_save_file и др.
//     регистрируются через vfs_register).
//   * vfs_stat/vfs_fstat/vfs_mkdir — новый срез VFS для stat-подобных ops.
//   * vfs_dup/vfs_dup2 — refcount на fd_entry.
//   * vfs_fcntl — F_GETFL/F_SETFL/F_DUPFD.
// Вся реализация остаётся single-thread-safe (на этапе 40 у нас ещё нет
// pthread_create, поэтому без atomic'ов).
// ============================================================================
#include "vfs.h"
#include "serial.h"
#include "syscall.h"   // AOS_O_* константы должны совпадать с VFS_O_*
// [этап 46] sandboxing: mount-ns gate
#include "task.h"
#include "ns.h"

// ---- Внешние хуки ФС (реализуются в fat32.c) -------------------------------
// Создание нового файла. Возвращает размер (или 0 для только-touch), -1 при ошибке.
extern int64_t fat32_create_file(const char* name, const void* data, uint64_t size);
// Upsert: перезапись или создание.
extern int64_t fat32_save_file(const char* name, const void* data, uint64_t size);
// Флаг монтирования — FAT32 должен быть смонтирован для create/save.
extern int     fat32_is_mounted(void);
// Создание каталога (может отсутствовать в 40; плейсхолдер — возвращает -1).
extern int     fat32_mkdir(const char* path);

static vfs_node_t nodes[VFS_MAX_FILES];
static int        node_count = 0;

typedef struct {
    vfs_fd_kind_t kind;
    int           refcount;
    int           flags;           // VFS_O_* биты (NONBLOCK, APPEND, RDWR-режим)
    int           cloexec;         // bit: FD_CLOEXEC
    // Объединение: либо файл, либо сокет, либо pipe.
    union {
        struct {
            vfs_node_t* node;
            uint64_t    pos;
        } file;
        struct {
            int socket_idx;
        } sock;
        struct {
            int pipe_idx;
        } pipe_;
    } u;
} fd_entry_t;

static fd_entry_t fd_table[VFS_MAX_FDS];

// ---- Локальные хелперы для строк -------------------------------------------
static int strcmp_local(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

// ---- Выделение свободного fd (для группы C тоже пригодится) ----------------
static int fd_alloc(void) {
    for (int fd = 0; fd < VFS_MAX_FDS; fd++) {
        if (fd_table[fd].kind == VFS_FD_UNUSED) return fd;
    }
    return -AOS_EMFILE;
}

// ---- Инициализация ---------------------------------------------------------
void vfs_init(void) {
    node_count = 0;
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        fd_table[i].kind     = VFS_FD_UNUSED;
        fd_table[i].refcount = 0;
        fd_table[i].flags    = 0;
        fd_table[i].cloexec  = 0;
    }
    serial_puts("[vfs] initialized (stage 40)\n");
}

int vfs_register(const vfs_node_t* tmpl) {
    if (node_count >= VFS_MAX_FILES) return -1;
    nodes[node_count] = *tmpl;
    // По умолчанию — обычный файл, если mode не задан.
    if (nodes[node_count].mode == 0) {
        nodes[node_count].mode = AOS_S_IFREG | 0644;
    }
    node_count++;
    return 0;
}

vfs_node_t* vfs_lookup(const char* name) {
    for (int i = 0; i < node_count; i++) {
        if (strcmp_local(nodes[i].name, name) == 0) return &nodes[i];
    }
    return 0;
}

int vfs_count(void) { return node_count; }

vfs_node_t* vfs_at(int idx) {
    if (idx < 0 || idx >= node_count) return 0;
    return &nodes[idx];
}

// ---- Общий открыватель -----------------------------------------------------
// Возвращает fd или отрицательный errno. Поддерживает O_CREAT, O_EXCL, O_TRUNC.
int vfs_open_flags(const char* name, int flags) {
    if (!name) return -AOS_EINVAL;

    // [этап 46] Mount-namespace gate. Если задача в isolated mnt-ns,
    // путь должен начинаться с одного из allow-prefixes (например
    // "/box/<id>/").
    if (!ns_mount_path_allowed(task_current(), name)) return -AOS_EACCES;

    vfs_node_t* n = vfs_lookup(name);

    // Запрос создания + несуществующий файл → fat32_create_file.
    if (!n && (flags & VFS_O_CREAT)) {
        if (!fat32_is_mounted()) return -AOS_ENODEV;
        int64_t r = fat32_create_file(name, 0, 0);
        if (r < 0) return -AOS_EACCES;
        n = vfs_lookup(name);          // fat32_create_file зарегистрирует узел
        if (!n) return -AOS_EACCES;
    } else if (n && (flags & VFS_O_CREAT) && (flags & VFS_O_EXCL)) {
        return -AOS_EEXIST;
    } else if (!n) {
        return -AOS_ENOENT;
    }

    // Проверка типа: на каталог открытие не разрешено как на обычный файл
    // (для этого есть отдельные opendir и т.п., но на этапе 40 их нет).
    if ((n->mode & AOS_S_IFMT) == AOS_S_IFDIR) {
        if (flags & (VFS_O_WRONLY | VFS_O_RDWR)) return -AOS_EISDIR;
    }

    // Truncate запрошен — сбросить размер. Реализация через fat32_save_file(name,"",0).
    if ((flags & VFS_O_TRUNC) && (flags & (VFS_O_WRONLY | VFS_O_RDWR))) {
        if (fat32_is_mounted()) {
            int64_t r = fat32_save_file(name, 0, 0);
            if (r < 0) return -AOS_EACCES;
            n = vfs_lookup(name);
            if (!n) return -AOS_ENOENT;
        }
    }

    int fd = fd_alloc();
    if (fd < 0) return fd;
    fd_entry_t* e = &fd_table[fd];
    e->kind              = VFS_FD_FILE;
    e->refcount          = 1;
    e->flags             = flags;
    e->cloexec           = (flags & 0x80000) ? 1 : 0; // O_CLOEXEC=0x80000
    e->u.file.node       = n;
    // O_APPEND → начальная позиция в конце.
    e->u.file.pos        = (flags & VFS_O_APPEND) ? n->size : 0;
    return fd;
}

// ---- Legacy точка входа (оставлена для совместимости) ----------------------
int vfs_open(const char* name) {
    int r = vfs_open_flags(name, VFS_O_RDONLY);
    if (r < 0) return -1;              // legacy-конвенция: -1 вместо errno
    return r;
}

int vfs_close(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || fd_table[fd].kind == VFS_FD_UNUSED) {
        return -AOS_EBADF;
    }
    fd_entry_t* e = &fd_table[fd];
    e->refcount--;
    if (e->refcount <= 0) {
        e->kind = VFS_FD_UNUSED;
        e->u.file.node = 0;
        e->flags = 0;
        e->cloexec = 0;
    }
    return 0;
}

int64_t vfs_read(int fd, void* buf, uint64_t len) {
    if (fd < 0 || fd >= VFS_MAX_FDS || fd_table[fd].kind != VFS_FD_FILE) {
        return -AOS_EBADF;
    }
    fd_entry_t* e = &fd_table[fd];
    vfs_node_t* n = e->u.file.node;
    if (!n || !n->read) return -AOS_EINVAL;
    // Write-only fd — нельзя читать.
    int acc = e->flags & VFS_O_ACCMODE;
    if (acc == VFS_O_WRONLY) return -AOS_EBADF;
    if (e->u.file.pos >= n->size) return 0;      // EOF
    uint64_t avail = n->size - e->u.file.pos;
    if (len > avail) len = avail;
    int64_t got = n->read(n, e->u.file.pos, len, buf);
    if (got > 0) e->u.file.pos += (uint64_t)got;
    return got;
}

int64_t vfs_write(int fd, const void* buf, uint64_t len) {
    if (fd < 0 || fd >= VFS_MAX_FDS || fd_table[fd].kind != VFS_FD_FILE) {
        return -AOS_EBADF;
    }
    fd_entry_t* e = &fd_table[fd];
    vfs_node_t* n = e->u.file.node;
    if (!n) return -AOS_EINVAL;
    int acc = e->flags & VFS_O_ACCMODE;
    if (acc == VFS_O_RDONLY) return -AOS_EBADF;
    // O_APPEND: каждая запись идёт в конец.
    if (e->flags & VFS_O_APPEND) e->u.file.pos = n->size;

    // Если у узла есть native write — используем. Иначе fallback через
    // fat32_save_file (перезапись всего файла; работает только для pos=0).
    if (n->write) {
        int64_t w = n->write(n, e->u.file.pos, len, buf);
        if (w > 0) e->u.file.pos += (uint64_t)w;
        return w;
    }
    // Fallback: допускается, только если пишем с pos=0 и фиксированный размер.
    if (e->u.file.pos == 0 && fat32_is_mounted()) {
        int64_t r = fat32_save_file(n->name, buf, len);
        if (r >= 0) {
            n->size = len;
            e->u.file.pos = (uint64_t)r;
        }
        return r;
    }
    return -AOS_EACCES;
}

int64_t vfs_seek(int fd, int64_t off, int whence) {
    if (fd < 0 || fd >= VFS_MAX_FDS || fd_table[fd].kind != VFS_FD_FILE) {
        return -AOS_EBADF;
    }
    fd_entry_t* e = &fd_table[fd];
    vfs_node_t* n = e->u.file.node;
    if (!n) return -AOS_EINVAL;
    int64_t newpos = 0;
    switch (whence) {
    case VFS_SEEK_SET: newpos = off; break;
    case VFS_SEEK_CUR: newpos = (int64_t)e->u.file.pos + off; break;
    case VFS_SEEK_END: newpos = (int64_t)n->size + off; break;
    default: return -AOS_EINVAL;
    }
    if (newpos < 0) return -AOS_EINVAL;
    e->u.file.pos = (uint64_t)newpos;
    return newpos;
}

int64_t vfs_size_fd(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || fd_table[fd].kind != VFS_FD_FILE) {
        return -AOS_EBADF;
    }
    return (int64_t)fd_table[fd].u.file.node->size;
}

// ============================================================================
// stat / fstat / mkdir
// ============================================================================
static void fill_stat(vfs_stat_t* st, vfs_node_t* n) {
    // Обнуляем, чтобы не было мусора.
    for (uint32_t i = 0; i < sizeof(vfs_stat_t); i++) ((uint8_t*)st)[i] = 0;
    st->st_dev     = 1;
    st->st_ino     = (uint64_t)(uintptr_t)n;
    st->st_nlink   = 1;
    st->st_mode    = n->mode ? n->mode : (AOS_S_IFREG | 0644);
    st->st_uid     = 0;
    st->st_gid     = 0;
    st->st_size    = (int64_t)n->size;
    st->st_blksize = 512;
    st->st_blocks  = (int64_t)((n->size + 511) / 512);
}

int vfs_stat(const char* path, vfs_stat_t* st) {
    if (!path || !st) return -AOS_EFAULT;
    vfs_node_t* n = vfs_lookup(path);
    if (!n) return -AOS_ENOENT;
    fill_stat(st, n);
    return 0;
}

int vfs_fstat(int fd, vfs_stat_t* st) {
    if (!st) return -AOS_EFAULT;
    if (fd < 0 || fd >= VFS_MAX_FDS || fd_table[fd].kind != VFS_FD_FILE) {
        return -AOS_EBADF;
    }
    fd_entry_t* e = &fd_table[fd];
    if (!e->u.file.node) return -AOS_EBADF;
    fill_stat(st, e->u.file.node);
    return 0;
}

int vfs_mkdir(const char* path, uint32_t mode) {
    (void)mode;
    if (!path) return -AOS_EFAULT;
    // На этапе 40 FAT32-реализация mkdir обычно отсутствует.  Ядро возвращает
    // ENOSYS — userspace получает понятный результат.
    int r = fat32_mkdir(path);
    return (r == 0) ? 0 : -AOS_ENOSYS;
}

// ============================================================================
// dup / dup2 / fcntl
// ============================================================================
int vfs_dup(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || fd_table[fd].kind == VFS_FD_UNUSED) {
        return -AOS_EBADF;
    }
    int nfd = fd_alloc();
    if (nfd < 0) return nfd;
    // Полное копирование entry; refcount источника увеличиваем,
    // nfd получает свою копию и удерживает ту же file/socket структуру.
    fd_table[nfd] = fd_table[fd];
    fd_table[fd].refcount++;
    fd_table[nfd].refcount = fd_table[fd].refcount;
    return nfd;
}

int vfs_dup2(int oldfd, int newfd) {
    if (oldfd < 0 || oldfd >= VFS_MAX_FDS || newfd < 0 || newfd >= VFS_MAX_FDS) {
        return -AOS_EBADF;
    }
    if (fd_table[oldfd].kind == VFS_FD_UNUSED) return -AOS_EBADF;
    if (oldfd == newfd) return newfd;
    // Закрыть newfd если был занят.
    if (fd_table[newfd].kind != VFS_FD_UNUSED) vfs_close(newfd);
    fd_table[newfd] = fd_table[oldfd];
    fd_table[oldfd].refcount++;
    fd_table[newfd].refcount = fd_table[oldfd].refcount;
    return newfd;
}

int vfs_fcntl(int fd, int cmd, long arg) {
    if (fd < 0 || fd >= VFS_MAX_FDS || fd_table[fd].kind == VFS_FD_UNUSED) {
        return -AOS_EBADF;
    }
    fd_entry_t* e = &fd_table[fd];
    switch (cmd) {
    case AOS_F_DUPFD:
        // Дублирование с минимальным номером не меньше arg.
        for (int f = (int)arg; f < VFS_MAX_FDS; f++) {
            if (fd_table[f].kind == VFS_FD_UNUSED) {
                fd_table[f] = *e;
                e->refcount++;
                fd_table[f].refcount = e->refcount;
                return f;
            }
        }
        return -AOS_EMFILE;
    case AOS_F_GETFD:
        return e->cloexec ? 1 : 0;
    case AOS_F_SETFD:
        e->cloexec = (arg & 1) ? 1 : 0;
        return 0;
    case AOS_F_GETFL:
        return e->flags;
    case AOS_F_SETFL:
        // Разрешаем менять только O_NONBLOCK и O_APPEND.
        e->flags = (e->flags & ~(VFS_O_NONBLOCK | VFS_O_APPEND))
                 | ((int)arg & (VFS_O_NONBLOCK | VFS_O_APPEND));
        return 0;
    default:
        return -AOS_EINVAL;
    }
}

// ============================================================================
// socket-fd helpers (используются группой C — stubs до реализации sockets)
// ============================================================================
int vfs_alloc_socket_fd(int socket_idx) {
    int fd = fd_alloc();
    if (fd < 0) return fd;
    fd_entry_t* e = &fd_table[fd];
    e->kind              = VFS_FD_SOCKET;
    e->refcount          = 1;
    e->flags             = 0;
    e->cloexec           = 0;
    e->u.sock.socket_idx = socket_idx;
    return fd;
}

int vfs_fd_kind(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS) return VFS_FD_UNUSED;
    return fd_table[fd].kind;
}

int vfs_fd_socket_idx(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || fd_table[fd].kind != VFS_FD_SOCKET) return -1;
    return fd_table[fd].u.sock.socket_idx;
}

int vfs_fd_flags(int fd) {
    if (fd < 0 || fd >= VFS_MAX_FDS || fd_table[fd].kind == VFS_FD_UNUSED) return -1;
    return fd_table[fd].flags;
}

int vfs_fd_set_flags(int fd, int flags) {
    if (fd < 0 || fd >= VFS_MAX_FDS || fd_table[fd].kind == VFS_FD_UNUSED) return -AOS_EBADF;
    fd_table[fd].flags = flags;
    return 0;
}

// ============================================================================
// Плейсхолдер fat32_mkdir (этап 40 не реализует mkdir в FAT32 — слишком
// большой скоуп; символ даётся weak, чтобы fat32.c мог его переопределить).
// ============================================================================
__attribute__((weak)) int fat32_mkdir(const char* path) {
    (void)path;
    return -1;  // нет реализации
}

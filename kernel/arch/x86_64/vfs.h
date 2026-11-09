// ============================================================================
// vfs.h — минимальная VFS для AMPLIFY-OS x86_64 (этап 37.5a).
// ----------------------------------------------------------------------------
// Цель: дать ядру и user-mode универсальный API для чтения файлов из
// initrd. На 37.5a поддерживается ТОЛЬКО плоский список файлов в root —
// без вложенных директорий, без записи (всё это придёт в 37.5b с FAT32).
//
// FD-таблица — глобальная, единая (single-task на сейчас не критично, но
// в 37.4d/5b сделаем per-process).
// ============================================================================
#ifndef AOS_X86_64_VFS_H
#define AOS_X86_64_VFS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define VFS_MAX_NAME    64
#define VFS_MAX_FILES   256
#define VFS_MAX_FDS     32

#define VFS_SEEK_SET    0
#define VFS_SEEK_CUR    1
#define VFS_SEEK_END    2

// ---- Этап 40: флаги открытия (зеркало libc fcntl.h) ----------------------
#define VFS_O_RDONLY    0x0000
#define VFS_O_WRONLY    0x0001
#define VFS_O_RDWR      0x0002
#define VFS_O_ACCMODE   0x0003
#define VFS_O_CREAT     0x0040
#define VFS_O_EXCL      0x0080
#define VFS_O_TRUNC     0x0200
#define VFS_O_APPEND    0x0400
#define VFS_O_NONBLOCK  0x0800

// ---- Этап 40: типы fd (для будущих socket/pipe fd) -----------------------
typedef enum {
    VFS_FD_UNUSED = 0,
    VFS_FD_FILE,
    VFS_FD_SOCKET,
    VFS_FD_PIPE
} vfs_fd_kind_t;

typedef struct vfs_node {
    char     name[VFS_MAX_NAME];
    uint64_t size;
    uint32_t mode;          // этап 40: биты S_IFREG/S_IFDIR + права (неиспользуемо пока как permission check)
    int64_t  (*read)(struct vfs_node*, uint64_t off, uint64_t len, void* buf);
    int64_t  (*write)(struct vfs_node*, uint64_t off, uint64_t len, const void* buf);
    int      (*truncate)(struct vfs_node*, uint64_t new_size);
    void*    impl;          // initrd: указатель на raw данные файла
} vfs_node_t;

void        vfs_init(void);
int         vfs_register(const vfs_node_t* tmpl);   // 0 при успехе
vfs_node_t* vfs_lookup(const char* name);
int         vfs_count(void);
vfs_node_t* vfs_at(int idx);

// FD-API (используется и kernel'ом, и user-mode через syscalls).
int     vfs_open(const char* name);                      // legacy: RDONLY
int     vfs_open_flags(const char* name, int flags);     // этап 40: полные флаги
int     vfs_close(int fd);
int64_t vfs_read(int fd, void* buf, uint64_t len);       // <0 при ошибке
int64_t vfs_write(int fd, const void* buf, uint64_t len);// этап 40
int64_t vfs_seek(int fd, int64_t off, int whence);
int64_t vfs_size_fd(int fd);

// ---- Этап 40: stat, mkdir, dup, fcntl --------------------------------------
// Минимальный aos_stat (layout дублирует syscall.h aos_stat_t).
typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    int64_t  st_atime;
    int64_t  st_atime_nsec;
    int64_t  st_mtime;
    int64_t  st_mtime_nsec;
    int64_t  st_ctime;
    int64_t  st_ctime_nsec;
    int64_t  __unused[3];
} vfs_stat_t;

int     vfs_stat (const char* path, vfs_stat_t* st);
int     vfs_fstat(int fd, vfs_stat_t* st);
int     vfs_mkdir(const char* path, uint32_t mode);

int     vfs_dup  (int fd);
int     vfs_dup2 (int oldfd, int newfd);
int     vfs_fcntl(int fd, int cmd, long arg);

// ---- Этап 40: регистрация socket fd (для group C) --------------------------
// Выделяет новый fd с kind = VFS_FD_SOCKET, сохраняет socket_idx. -1 при error.
int     vfs_alloc_socket_fd(int socket_idx);
int     vfs_fd_kind(int fd);                 // один из VFS_FD_*; UNUSED если fd невалиден
int     vfs_fd_socket_idx(int fd);           // socket_idx; -1 если не SOCKET
int     vfs_fd_flags(int fd);                // текущие флаги (для O_NONBLOCK/O_APPEND)
int     vfs_fd_set_flags(int fd, int flags); // устанавливает bitmask

#endif

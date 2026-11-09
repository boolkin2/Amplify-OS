// ============================================================================
// vfs.h - Virtual File System для MyOS (Расширенная версия — Этап 17)
// ============================================================================
// Абстрактный слой файловой системы с поддержкой:
//   - Монтирования нескольких ФС
//   - Навигации по дереву каталогов (разрешение путей)
//   - Создания/удаления файлов и каталогов
//   - Операции stat для получения метаданных
// ============================================================================

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// ============================================================================
// Константы VFS
// ============================================================================

// Максимальная длина имени файла
#define VFS_MAX_FILENAME    256

// Максимальная длина полного пути
#define VFS_MAX_PATH        1024

// Максимальное количество открытых файлов
#define VFS_MAX_OPEN_FILES  32

// Максимальное количество точек монтирования
#define VFS_MAX_MOUNTS      8

// Типы файлов
#define VFS_FILE            0x01    // Обычный файл
#define VFS_DIRECTORY       0x02    // Директория
#define VFS_CHARDEVICE      0x03    // Символьное устройство
#define VFS_BLOCKDEVICE     0x04    // Блочное устройство
#define VFS_PIPE            0x05    // Pipe
#define VFS_SYMLINK         0x06    // Символическая ссылка
#define VFS_MOUNTPOINT      0x08    // Точка монтирования

// Флаги открытия файла
#define VFS_O_RDONLY        0x01    // Только чтение
#define VFS_O_WRONLY        0x02    // Только запись
#define VFS_O_RDWR          0x03    // Чтение и запись
#define VFS_O_APPEND        0x04    // Добавление в конец
#define VFS_O_CREAT         0x08    // Создать если не существует
#define VFS_O_TRUNC         0x10    // Обрезать до 0 при открытии

// Позиции для seek
#define VFS_SEEK_SET        0       // От начала файла
#define VFS_SEEK_CUR        1       // От текущей позиции
#define VFS_SEEK_END        2       // От конца файла

// Коды ошибок VFS
#define VFS_OK              0       // Успех
#define VFS_ERR_NOENT       -1      // Файл/каталог не найден
#define VFS_ERR_EXIST       -2      // Файл уже существует
#define VFS_ERR_NOTDIR      -3      // Не каталог
#define VFS_ERR_ISDIR       -4      // Это каталог (нельзя как файл)
#define VFS_ERR_NOSPC       -5      // Нет свободного места
#define VFS_ERR_IO          -6      // Ошибка ввода-вывода
#define VFS_ERR_NOPERM      -7      // Нет прав доступа
#define VFS_ERR_NOMEM       -8      // Недостаточно памяти
#define VFS_ERR_BUSY        -9      // Ресурс занят
#define VFS_ERR_NOFD        -10     // Нет свободных дескрипторов
#define VFS_ERR_INVAL       -11     // Неверный аргумент

// ============================================================================
// Структуры VFS
// ============================================================================

// Прототипы структур
struct vfs_node;
struct vfs_dirent;

// Указатели на функции для операций с файлами
typedef uint32_t (*vfs_read_t)(struct vfs_node*, uint32_t offset, uint32_t size, uint8_t* buffer);
typedef uint32_t (*vfs_write_t)(struct vfs_node*, uint32_t offset, uint32_t size, uint8_t* buffer);
typedef void (*vfs_open_t)(struct vfs_node*);
typedef void (*vfs_close_t)(struct vfs_node*);
typedef struct vfs_dirent* (*vfs_readdir_t)(struct vfs_node*, uint32_t index);
typedef struct vfs_node* (*vfs_finddir_t)(struct vfs_node*, const char* name);

// Новые операции для записи
typedef int (*vfs_create_t)(struct vfs_node* parent, const char* name, uint32_t flags);
typedef int (*vfs_mkdir_t)(struct vfs_node* parent, const char* name);
typedef int (*vfs_unlink_t)(struct vfs_node* parent, const char* name);
typedef int (*vfs_truncate_t)(struct vfs_node* node, uint32_t new_size);

// Узел файловой системы (файл или директория)
typedef struct vfs_node {
    char name[VFS_MAX_FILENAME];    // Имя файла
    uint32_t mask;                  // Права доступа
    uint32_t uid;                   // User ID
    uint32_t gid;                   // Group ID
    uint32_t flags;                 // Тип файла (VFS_FILE, VFS_DIRECTORY и т.д.)
    uint32_t inode;                 // Номер inode
    uint32_t length;                // Размер файла в байтах
    uint32_t impl;                  // Зависит от реализации ФС
    
    // Указатели на функции (базовые)
    vfs_read_t read;
    vfs_write_t write;
    vfs_open_t open;
    vfs_close_t close;
    vfs_readdir_t readdir;
    vfs_finddir_t finddir;
    
    // Указатели на функции (расширенные — Этап 17)
    vfs_create_t create;            // Создание файла в каталоге
    vfs_mkdir_t mkdir;              // Создание подкаталога
    vfs_unlink_t unlink;            // Удаление файла/каталога
    vfs_truncate_t truncate;        // Изменение размера файла
    
    struct vfs_node* ptr;           // Используется для symlinks и mountpoints
    struct vfs_node* parent;        // Указатель на родительский каталог
    void* fs_data;                  // Приватные данные драйвера ФС
} vfs_node_t;

// Запись директории
typedef struct vfs_dirent {
    char name[VFS_MAX_FILENAME];    // Имя файла
    uint32_t inode;                 // Номер inode
} vfs_dirent_t;

// Дескриптор открытого файла
typedef struct {
    vfs_node_t* node;               // Узел файла
    uint32_t position;              // Текущая позиция в файле
    uint32_t flags;                 // Флаги открытия
    bool in_use;                    // Используется ли дескриптор
} vfs_fd_t;

// Информация о файле (аналог stat)
typedef struct {
    uint32_t type;                  // Тип файла (VFS_FILE, VFS_DIRECTORY и т.д.)
    uint32_t size;                  // Размер в байтах
    uint32_t inode;                 // Номер inode
    uint32_t mask;                  // Права доступа
    uint32_t uid;                   // User ID
    uint32_t gid;                   // Group ID
} vfs_stat_info_t;

// Точка монтирования
typedef struct {
    char path[VFS_MAX_PATH];        // Путь точки монтирования (напр. "/disk")
    vfs_node_t* root;               // Корневой узел файловой системы
    bool in_use;                    // Используется ли
    char fs_type[32];               // Тип ФС (напр. "fat32", "initrd")
} vfs_mount_t;

// ============================================================================
// Функции VFS (базовые — сохранены для совместимости)
// ============================================================================

// Инициализация VFS
void vfs_init(void);

// Установка корневой файловой системы
void vfs_set_root(vfs_node_t* root);

// Получение корневой файловой системы
vfs_node_t* vfs_get_root(void);

// Чтение из файла
uint32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);

// Запись в файл
uint32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer);

// Открытие файла
void vfs_open(vfs_node_t* node, uint32_t flags);

// Закрытие файла
void vfs_close(vfs_node_t* node);

// Чтение записи директории по индексу
vfs_dirent_t* vfs_readdir(vfs_node_t* node, uint32_t index);

// Поиск файла в директории по имени
vfs_node_t* vfs_finddir(vfs_node_t* node, const char* name);

// ============================================================================
// Функции VFS (файловые дескрипторы)
// ============================================================================

// Открытие файла по пути (возвращает файловый дескриптор)
int vfs_open_file(const char* path, uint32_t flags);

// Чтение из файлового дескриптора
int vfs_read_file(int fd, void* buffer, size_t size);

// Запись в файловый дескриптор
int vfs_write_file(int fd, const void* buffer, size_t size);

// Закрытие файлового дескриптора
int vfs_close_file(int fd);

// Перемещение позиции в файле
int vfs_seek_file(int fd, int offset, int whence);

// Получение размера файла
int vfs_get_filesize(int fd);

// ============================================================================
// Новые функции VFS (Этап 17)
// ============================================================================

// Разрешение пути — преобразование строки пути в VFS-узел
// Параметры:
//   path — полный путь (например "/disk/vault/file.txt")
// Возвращает указатель на VFS-узел или NULL
vfs_node_t* vfs_resolve_path(const char* path);

// Разрешение пути с получением родительского каталога
int vfs_resolve_parent(const char* path, struct vfs_node** parent_out, char* name_out);

// Нормализация пути: разворачивает "." и ".." в абсолютном пути.
// Вход: in  — абсолютный путь (начинается с '/'), может содержать //, ., ..
// Выход: out — нормализованный путь, всегда начинается с '/'.
// out буфер должен быть размером ≥ VFS_MAX_PATH.
// Возвращает длину нормализованного пути или -1 при ошибке (не абсолютный путь).
int vfs_normalize_path(char* out, const char* in);

// Построить абсолютный путь из base (cwd) и path.
// Если path абсолютный (начинается с '/') — игнорирует base.
// Иначе склеивает "base/path" и нормализует.
// Возвращает длину результата или -1 при ошибке (слишком длинно).
int vfs_join_path(char* out, const char* base, const char* path);

// Монтирование файловой системы
// Параметры:
//   path    — точка монтирования (например "/disk")
//   root    — корневой узел монтируемой ФС
//   fs_type — тип ФС ("fat32", "initrd" и т.д.)
// Возвращает 0 при успехе
int vfs_mount(const char* path, vfs_node_t* root, const char* fs_type);

// Размонтирование файловой системы
int vfs_umount(const char* path);

// Получение информации о файле (stat)
int vfs_stat(const char* path, vfs_stat_info_t* stat_out);

// Создание файла
// Параметры:
//   path  — полный путь нового файла
//   flags — флаги (VFS_FILE)
// Возвращает 0 при успехе
int vfs_create_file(const char* path, uint32_t flags);

// Создание каталога
int vfs_mkdir(const char* path);

// Удаление файла или каталога
int vfs_unlink(const char* path);

// Получение информации о точке монтирования по индексу
const vfs_mount_t* vfs_get_mount(int index);

// Количество активных точек монтирования
int vfs_get_mount_count(void);

#endif // VFS_H

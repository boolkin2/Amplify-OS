// ============================================================================
// vfs.c - Реализация Virtual File System для MyOS (Расширенная — Этап 17)
// ============================================================================
// Добавлено:
//   - Таблица точек монтирования
//   - Разрешение путей (навигация по /dir1/dir2/file)
//   - Создание/удаление файлов и каталогов
//   - Операция stat
// ============================================================================

#include "vfs.h"
#include "../memory/heap.h"
#include "../memory/memory.h"
#include "../vga.h"
#include "../security/security.h"

// ============================================================================
// Глобальные переменные
// ============================================================================

// Корневая файловая система (InitRD)
static vfs_node_t* vfs_root = NULL;

// Таблица открытых файлов
static vfs_fd_t vfs_fd_table[VFS_MAX_OPEN_FILES];

// Таблица точек монтирования
static vfs_mount_t vfs_mounts[VFS_MAX_MOUNTS];

// ============================================================================
// Вспомогательные функции для работы со строками
// ============================================================================

// Длина строки
static int vfs_strlen(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

// Сравнение строк
static int vfs_strcmp(const char* s1, const char* s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

// Сравнение N символов
static int vfs_strncmp(const char* s1, const char* s2, int n) {
    while (n > 0 && *s1 && (*s1 == *s2)) { s1++; s2++; n--; }
    if (n == 0) return 0;
    return *(unsigned char*)s1 - *(unsigned char*)s2;
}

// Копирование строки
static void vfs_strcpy(char* dst, const char* src) {
    while (*src) { *dst++ = *src++; }
    *dst = '\0';
}

// Копирование N символов
static void vfs_strncpy(char* dst, const char* src, int n) {
    int i;
    for (i = 0; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

// Поиск свободного файлового дескриптора
static int vfs_alloc_fd(void) {
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        if (!vfs_fd_table[i].in_use) {
            return i;
        }
    }
    return -1;  // Нет свободных дескрипторов
}

// ============================================================================
// Инициализация VFS
// ============================================================================

void vfs_init(void) {
    vfs_root = NULL;
    
    // Инициализируем таблицу файловых дескрипторов
    for (int i = 0; i < VFS_MAX_OPEN_FILES; i++) {
        vfs_fd_table[i].node = NULL;
        vfs_fd_table[i].position = 0;
        vfs_fd_table[i].flags = 0;
        vfs_fd_table[i].in_use = false;
    }

    // Инициализируем таблицу точек монтирования
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        vfs_mounts[i].in_use = false;
        vfs_mounts[i].root = NULL;
        vfs_mounts[i].path[0] = '\0';
        vfs_mounts[i].fs_type[0] = '\0';
    }
}

// ============================================================================
// Базовые операции (совместимы с предыдущей версией)
// ============================================================================

void vfs_set_root(vfs_node_t* root) {
    vfs_root = root;
}

vfs_node_t* vfs_get_root(void) {
    return vfs_root;
}

uint32_t vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !node->read) {
        return 0;
    }
    return node->read(node, offset, size, buffer);
}

uint32_t vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !node->write) {
        return 0;
    }
    return node->write(node, offset, size, buffer);
}

void vfs_open(vfs_node_t* node, uint32_t flags) {
    (void)flags;
    if (node && node->open) {
        node->open(node);
    }
}

void vfs_close(vfs_node_t* node) {
    if (node && node->close) {
        node->close(node);
    }
}

vfs_dirent_t* vfs_readdir(vfs_node_t* node, uint32_t index) {
    if (!node || !node->readdir || !(node->flags & VFS_DIRECTORY)) {
        return NULL;
    }
    return node->readdir(node, index);
}

vfs_node_t* vfs_finddir(vfs_node_t* node, const char* name) {
    if (!node || !node->finddir || !(node->flags & VFS_DIRECTORY)) {
        return NULL;
    }
    return node->finddir(node, name);
}

// ============================================================================
// Монтирование файловых систем
// ============================================================================

// Монтирование ФС на указанный путь
int vfs_mount(const char* path, vfs_node_t* root, const char* fs_type) {
    if (!path || !root || !fs_type) {
        return VFS_ERR_INVAL;
    }

    // Ищем свободную точку монтирования
    int slot = -1;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!vfs_mounts[i].in_use) {
            slot = i;
            break;
        }
        // Проверяем, не смонтировано ли уже по этому пути
        if (vfs_strcmp(vfs_mounts[i].path, path) == 0) {
            return VFS_ERR_BUSY;
        }
    }

    if (slot < 0) {
        return VFS_ERR_NOMEM;  // Нет свободных слотов
    }

    // Заполняем структуру точки монтирования
    vfs_strncpy(vfs_mounts[slot].path, path, VFS_MAX_PATH - 1);
    vfs_strncpy(vfs_mounts[slot].fs_type, fs_type, 31);
    vfs_mounts[slot].root = root;
    vfs_mounts[slot].in_use = true;

    return VFS_OK;
}

// Размонтирование ФС
int vfs_umount(const char* path) {
    if (!path) return VFS_ERR_INVAL;

    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_mounts[i].in_use && vfs_strcmp(vfs_mounts[i].path, path) == 0) {
            vfs_mounts[i].in_use = false;
            vfs_mounts[i].root = NULL;
            return VFS_OK;
        }
    }

    return VFS_ERR_NOENT;
}

// Получение информации о точке монтирования
const vfs_mount_t* vfs_get_mount(int index) {
    if (index < 0 || index >= VFS_MAX_MOUNTS) return NULL;
    if (!vfs_mounts[index].in_use) return NULL;
    return &vfs_mounts[index];
}

// Количество активных точек монтирования
int vfs_get_mount_count(void) {
    int count = 0;
    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (vfs_mounts[i].in_use) count++;
    }
    return count;
}

// ============================================================================
// Разрешение путей
// ============================================================================

// Поиск подходящей точки монтирования для пути
// Возвращает индекс в таблице монтирования или -1
static int vfs_find_mount(const char* path) {
    int best_match = -1;
    int best_len = 0;

    for (int i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!vfs_mounts[i].in_use) continue;

        int mount_len = vfs_strlen(vfs_mounts[i].path);

        // Проверяем, начинается ли путь с точки монтирования
        if (vfs_strncmp(path, vfs_mounts[i].path, mount_len) == 0) {
            // Путь должен быть точно равен или продолжаться '/'
            if (path[mount_len] == '\0' || path[mount_len] == '/') {
                if (mount_len > best_len) {
                    best_len = mount_len;
                    best_match = i;
                }
            }
        }
    }

    return best_match;
}

// Разрешение пути в VFS-узел
vfs_node_t* vfs_resolve_path(const char* path) {
    if (!path || path[0] == '\0') {
        return NULL;
    }

    // Путь "/" — корень
    if (path[0] == '/' && path[1] == '\0') {
        return vfs_root;
    }

    // Проверяем точки монтирования
    int mount_idx = vfs_find_mount(path);
    vfs_node_t* current;
    const char* remaining;

    if (mount_idx >= 0) {
        // Путь ведёт через точку монтирования
        current = vfs_mounts[mount_idx].root;
        int mount_len = vfs_strlen(vfs_mounts[mount_idx].path);
        remaining = path + mount_len;

        // Если путь = точка монтирования, вернуть её корень
        if (*remaining == '\0') {
            return current;
        }

        // Пропускаем '/' после точки монтирования
        if (*remaining == '/') {
            remaining++;
        }
    } else {
        // Путь от корневой ФС
        current = vfs_root;
        remaining = path;

        // Пропускаем начальный '/'
        if (*remaining == '/') {
            remaining++;
        }
    }

    if (!current) {
        return NULL;
    }

    // Если путь пустой после корня — вернуть корень
    if (*remaining == '\0') {
        return current;
    }

    // Разбираем путь по компонентам: "vault/docs/file.txt"
    char component[VFS_MAX_FILENAME];

    while (*remaining) {
        // Извлекаем следующий компонент пути
        int i = 0;
        while (*remaining && *remaining != '/' && i < VFS_MAX_FILENAME - 1) {
            component[i++] = *remaining++;
        }
        component[i] = '\0';

        // Пропускаем '/'
        if (*remaining == '/') {
            remaining++;
        }

        // Пропускаем пустые компоненты ("//")
        if (i == 0) continue;

        // "." — остаёмся в текущем каталоге.
        if (vfs_strcmp(component, ".") == 0) {
            continue;
        }

        // ".." — поднимаемся к родителю. У корневых и корне-подобных узлов
        // parent == self, так что "/.." == "/".
        if (vfs_strcmp(component, "..") == 0) {
            if (current->parent) {
                current = current->parent;
            }
            continue;
        }

        // Ищем компонент в текущей директории
        if (!(current->flags & VFS_DIRECTORY)) {
            return NULL;  // Текущий узел — не директория
        }

        vfs_node_t* next = vfs_finddir(current, component);
        if (!next) {
            return NULL;  // Компонент не найден
        }

        // Страховочное заполнение parent на случай, если драйвер ФС
        // этого не делает (initrd заполняет, у FAT32 — только корневой).
        if (!next->parent) {
            next->parent = current;
        }

        current = next;
    }

    return current;
}

// ============================================================================
// Нормализация пути: развернуть "." и ".." без обращения к ФС
// ============================================================================
// Работает по принципу стека компонентов: проходим по path,
// на "." ничего не делаем, на ".." откатываем последний компонент,
// иначе добавляем в конец. Результат всегда начинается с '/'.
int vfs_normalize_path(char* out, const char* in) {
    if (!out || !in) return -1;
    if (in[0] != '/') return -1;  // принимаем только абсолютные пути

    // Массив указателей на компоненты внутри out и их длины.
    // Храним всё in-place в out, строя его слева направо.
    int out_len = 0;
    out[0] = '\0';

    // Позиции начал компонентов в out, чтобы быстро "откатываться" на ".."
    int starts[64];       // до 64 уровней вложенности — разумно
    int depth = 0;

    const char* p = in;
    while (*p) {
        // Пропускаем подряд идущие '/'.
        while (*p == '/') p++;
        if (!*p) break;

        // Вырезаем компонент [p .. q).
        const char* q = p;
        while (*q && *q != '/') q++;
        int clen = (int)(q - p);

        if (clen == 1 && p[0] == '.') {
            // "." — ничего не делаем.
        } else if (clen == 2 && p[0] == '.' && p[1] == '.') {
            // ".." — снимаем последний компонент, если он есть.
            if (depth > 0) {
                out_len = starts[--depth];
                if (out_len > 0) out_len--;   // удаляем ведущий '/'
                out[out_len] = '\0';
            }
        } else {
            // Обычный компонент.
            if (depth >= 64) return -1;
            if (out_len + 1 + clen + 1 > VFS_MAX_PATH) return -1;
            starts[depth++] = out_len + 1;     // позиция после '/'
            out[out_len++] = '/';
            for (int i = 0; i < clen; i++) out[out_len++] = p[i];
            out[out_len] = '\0';
        }
        p = q;
    }

    // Пустой результат = корень.
    if (out_len == 0) {
        out[0] = '/';
        out[1] = '\0';
        return 1;
    }
    return out_len;
}

// ============================================================================
// Склейка base + path → нормализованный абсолютный путь
// ============================================================================
int vfs_join_path(char* out, const char* base, const char* path) {
    if (!out || !path) return -1;

    // Если path абсолютный — игнорируем base.
    if (path[0] == '/') {
        return vfs_normalize_path(out, path);
    }

    if (!base || base[0] != '/') return -1;

    // Склеиваем во временный буфер.
    char tmp[VFS_MAX_PATH];
    int bl = vfs_strlen(base);
    int pl = vfs_strlen(path);
    if (bl + 1 + pl + 1 > (int)sizeof(tmp)) return -1;

    int i = 0;
    for (int j = 0; j < bl; j++) tmp[i++] = base[j];
    // Вставляем разделитель, если base не заканчивается на '/'.
    if (i == 0 || tmp[i - 1] != '/') tmp[i++] = '/';
    for (int j = 0; j < pl; j++) tmp[i++] = path[j];
    tmp[i] = '\0';

    return vfs_normalize_path(out, tmp);
}

// Разрешение пути с получением родительского каталога
int vfs_resolve_parent(const char* path, vfs_node_t** parent_out, char* name_out) {
    if (!path || !parent_out || !name_out || path[0] == '\0') {
        return VFS_ERR_INVAL;
    }

    // Находим последний '/' в пути
    int last_slash = -1;
    int path_len = vfs_strlen(path);
    for (int i = path_len - 1; i >= 0; i--) {
        if (path[i] == '/') {
            last_slash = i;
            break;
        }
    }

    if (last_slash < 0) {
        // Нет '/' — ищем в корне
        *parent_out = vfs_root;
        vfs_strcpy(name_out, path);
        return VFS_OK;
    }

    // Извлекаем имя файла (после последнего '/')
    vfs_strcpy(name_out, path + last_slash + 1);

    if (name_out[0] == '\0') {
        return VFS_ERR_INVAL;  // Путь заканчивается на '/'
    }

    // Разрешаем путь к родительскому каталогу
    if (last_slash == 0) {
        // Родитель — корень "/"
        *parent_out = vfs_root;
    } else {
        // Копируем путь к родителю
        char parent_path[VFS_MAX_PATH];
        vfs_strncpy(parent_path, path, last_slash);
        parent_path[last_slash] = '\0';

        *parent_out = vfs_resolve_path(parent_path);
        if (!*parent_out) {
            return VFS_ERR_NOENT;
        }
    }

    // Проверяем, что родитель — директория
    if (!((*parent_out)->flags & VFS_DIRECTORY)) {
        return VFS_ERR_NOTDIR;
    }

    return VFS_OK;
}

// ============================================================================
// Новые операции (Этап 17)
// ============================================================================

// Получение информации о файле
int vfs_stat(const char* path, vfs_stat_info_t* stat_out) {
    if (!path || !stat_out) return VFS_ERR_INVAL;

    vfs_node_t* node = vfs_resolve_path(path);
    if (!node) return VFS_ERR_NOENT;

    stat_out->type = node->flags;
    stat_out->size = node->length;
    stat_out->inode = node->inode;
    stat_out->mask = node->mask;
    stat_out->uid = node->uid;
    stat_out->gid = node->gid;

    return VFS_OK;
}

// Создание файла
int vfs_create_file(const char* path, uint32_t flags) {
    vfs_node_t* parent = NULL;
    char name[VFS_MAX_FILENAME];

    int rc = vfs_resolve_parent(path, &parent, name);
    if (rc != VFS_OK) return rc;

    // Проверяем, не существует ли уже
    vfs_node_t* existing = vfs_finddir(parent, name);
    if (existing) return VFS_ERR_EXIST;

    // Вызываем create callback родительского каталога
    if (!parent->create) return VFS_ERR_NOPERM;  // ФС не поддерживает создание
    (void)flags;
    return parent->create(parent, name, VFS_FILE);
}

// Создание каталога
int vfs_mkdir(const char* path) {
    vfs_node_t* parent = NULL;
    char name[VFS_MAX_FILENAME];

    int rc = vfs_resolve_parent(path, &parent, name);
    if (rc != VFS_OK) return rc;

    // Проверяем, не существует ли уже
    vfs_node_t* existing = vfs_finddir(parent, name);
    if (existing) return VFS_ERR_EXIST;

    // Вызываем mkdir callback
    if (!parent->mkdir) return VFS_ERR_NOPERM;
    return parent->mkdir(parent, name);
}

// Удаление файла или каталога
int vfs_unlink(const char* path) {
    vfs_node_t* parent = NULL;
    char name[VFS_MAX_FILENAME];

    int rc = vfs_resolve_parent(path, &parent, name);
    if (rc != VFS_OK) return rc;

    // Проверяем, существует ли
    vfs_node_t* target = vfs_finddir(parent, name);
    if (!target) return VFS_ERR_NOENT;

    // Вызываем unlink callback
    if (!parent->unlink) return VFS_ERR_NOPERM;
    return parent->unlink(parent, name);
}

// ============================================================================
// Файловые дескрипторы (обновлённые с разрешением путей)
// ============================================================================

// Открытие файла по пути
int vfs_open_file(const char* path, uint32_t flags) {
    if (!path) {
        return VFS_ERR_INVAL;
    }

    // Разрешаем путь
    vfs_node_t* node = vfs_resolve_path(path);

    // Если файл не найден и установлен O_CREAT — создаём
    if (!node && (flags & VFS_O_CREAT)) {
        int rc = vfs_create_file(path, VFS_FILE);
        if (rc != VFS_OK) return rc;
        node = vfs_resolve_path(path);
    }

    if (!node) {
        return VFS_ERR_NOENT;
    }

    // Нельзя открыть каталог как файл
    if (node->flags & VFS_DIRECTORY) {
        return VFS_ERR_ISDIR;
    }

    // ----------------------------------------------------------------
    // Этап 32: проверка прав доступа.
    // Если у узла выставлены mask/uid/gid (mask != 0) — проверяем,
    // что текущий пользователь имеет нужный набор бит. Если mask=0
    // (узел старый, до Этапа 32) — пропускаем проверку для совместимости.
    // ----------------------------------------------------------------
    if (node->mask != 0) {
        sec_perm_t need = SEC_PERM_READ;
        if (flags & (VFS_O_WRONLY | VFS_O_RDWR | VFS_O_APPEND | VFS_O_TRUNC)) {
            need = (sec_perm_t)(SEC_PERM_READ | SEC_PERM_WRITE);
        }
        if (!sec_check_perm_current(node->uid, node->gid, node->mask, need)) {
            return VFS_ERR_NOPERM;
        }
    }

    // Выделяем файловый дескриптор
    int fd = vfs_alloc_fd();
    if (fd < 0) {
        return VFS_ERR_NOFD;
    }

    // Инициализируем дескриптор
    vfs_fd_table[fd].node = node;
    vfs_fd_table[fd].position = 0;
    vfs_fd_table[fd].flags = flags;
    vfs_fd_table[fd].in_use = true;

    // Если O_TRUNC — обрезаем файл
    if ((flags & VFS_O_TRUNC) && node->truncate) {
        node->truncate(node, 0);
    }

    // Если O_APPEND — ставим позицию в конец
    if (flags & VFS_O_APPEND) {
        vfs_fd_table[fd].position = node->length;
    }

    // Вызываем open callback
    vfs_open(node, flags);

    return fd;
}

// Чтение из файлового дескриптора
int vfs_read_file(int fd, void* buffer, size_t size) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !vfs_fd_table[fd].in_use) {
        return -1;
    }
    
    vfs_fd_t* file = &vfs_fd_table[fd];
    
    // Проверяем права на чтение
    if (!(file->flags & VFS_O_RDONLY) && !(file->flags & VFS_O_RDWR)) {
        return -1;
    }
    
    // Читаем данные
    uint32_t bytes_read = vfs_read(file->node, file->position, size, (uint8_t*)buffer);
    
    // Обновляем позицию
    file->position += bytes_read;
    
    return bytes_read;
}

// Запись в файловый дескриптор
int vfs_write_file(int fd, const void* buffer, size_t size) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !vfs_fd_table[fd].in_use) {
        return -1;
    }
    
    vfs_fd_t* file = &vfs_fd_table[fd];
    
    // Проверяем права на запись
    if (!(file->flags & VFS_O_WRONLY) && !(file->flags & VFS_O_RDWR)) {
        return -1;
    }
    
    // Записываем данные
    uint32_t bytes_written = vfs_write(file->node, file->position, size, (uint8_t*)buffer);
    
    // Обновляем позицию
    file->position += bytes_written;
    
    return bytes_written;
}

// Закрытие файлового дескриптора
int vfs_close_file(int fd) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !vfs_fd_table[fd].in_use) {
        return -1;
    }
    
    vfs_close(vfs_fd_table[fd].node);
    
    vfs_fd_table[fd].in_use = false;
    vfs_fd_table[fd].node = NULL;
    vfs_fd_table[fd].position = 0;
    vfs_fd_table[fd].flags = 0;
    
    return 0;
}

// Перемещение позиции в файле
int vfs_seek_file(int fd, int offset, int whence) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !vfs_fd_table[fd].in_use) {
        return -1;
    }
    
    vfs_fd_t* file = &vfs_fd_table[fd];
    
    switch (whence) {
        case VFS_SEEK_SET:
            file->position = offset;
            break;
        case VFS_SEEK_CUR:
            file->position += offset;
            break;
        case VFS_SEEK_END:
            file->position = file->node->length + offset;
            break;
        default:
            return -1;
    }
    
    return file->position;
}

// Получение размера файла
int vfs_get_filesize(int fd) {
    if (fd < 0 || fd >= VFS_MAX_OPEN_FILES || !vfs_fd_table[fd].in_use) {
        return -1;
    }
    
    return vfs_fd_table[fd].node->length;
}

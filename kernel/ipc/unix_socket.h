// ============================================================================
// unix_socket.h — Unix Domain Sockets для MyOS (Этап 29)
// ----------------------------------------------------------------------------
// Локальные IPC-сокеты без сети. Две стороны соединения обмениваются
// данными через пару pipe-ов (по одному в каждом направлении). Именование —
// через реестр: сервер делает bind(name) + listen, клиент делает
// connect(name) → ядро сводит их в соединение и выдаёт обоим fd.
//
// Архитектура (упрощённая, kernel-only):
//
//   server:                        client:
//     fd = usock_bind("greet")       fd = usock_connect("greet")
//     usock_listen(fd)               usock_send(fd, ...)
//     peer = usock_accept(fd)        usock_recv(fd, ...)
//     usock_send(peer, ...)
//     usock_recv(peer, ...)
//
// Регистр g_sockets[16]: каждый элемент — «эндпойнт»:
//   • LISTENER — держит name, собирает очередь incoming connect-запросов.
//   • ENDPOINT — соединённая точка, имеет указатели на 2 pipe'а
//     (tx=in-to-peer, rx=peer-to-in) и pid пира.
//
// В отсутствие preemptive multitasking связь симулируется: connect
// сразу же создаёт пару connected endpoint'ов и делает их доступными
// через accept() без блокировки. Это достаточно для демонстрации
// IPC-паттернов и для будущего расширения после внедрения scheduler'а.
// ============================================================================

#ifndef UNIX_SOCKET_H
#define UNIX_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define USOCK_MAX          16         // Максимум одновременно активных fd
#define USOCK_NAME_LEN     32         // Длина имени сокета
#define USOCK_BUF_SIZE     2048       // Размер pipe-буфера для каждого направления

#define USOCK_INVALID_FD   -1

// Типы эндпойнтов
typedef enum {
    USOCK_FREE = 0,
    USOCK_LISTENER,        // Ждёт входящих соединений (bind + listen)
    USOCK_ENDPOINT,        // Соединённая точка (после accept / connect)
} usock_type_t;

// Инициализация подсистемы. Должна быть вызвана один раз из kernel_main.
void usock_init(void);

// Создать слушающий сокет по имени. Возвращает fd или USOCK_INVALID_FD.
int usock_bind(const char* name);

// Перевести в состояние "listen" (разрешить приём входящих).
// В нашей упрощённой реализации bind уже создаёт LISTENER — этот вызов
// для совместимости с Unix-API и возвращает 0 при успехе.
int usock_listen(int fd);

// Неблокирующе принять следующее входящее соединение. Возвращает fd
// нового ENDPOINT'а или USOCK_INVALID_FD, если нет ожидающих.
int usock_accept(int fd);

// Подключиться к слушателю по имени. Создаёт ENDPOINT на стороне клиента и
// ставит его в очередь accept() сервера. Возвращает fd клиентской стороны.
int usock_connect(const char* name);

// Отправить байты сквозь соединение. Возвращает число реально записанных.
size_t usock_send(int fd, const void* data, size_t len);

// Получить байты. Возвращает число прочитанных (0 если пусто или EOF).
size_t usock_recv(int fd, void* buf, size_t cap);

// Закрыть сокет (LISTENER или ENDPOINT). Освобождает связанные pipe-ы и
// пересылает EOF противоположной стороне (если была соединена).
void usock_close(int fd);

// Проверка: есть ли у сокета данные для чтения?
bool usock_has_data(int fd);

#endif // UNIX_SOCKET_H

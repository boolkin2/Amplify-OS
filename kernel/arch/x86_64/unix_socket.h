// ============================================================================
// unix_socket.h — Unix Domain Sockets для AMPLIFY-OS x86_64.  Порт.
// ----------------------------------------------------------------------------
// Двунаправленная связь через пару pipe'ов (один на каждое направление).
// Реестр имён: bind+listen vs connect.  Без preemption — connect+accept
// синхронно создают пару endpoint'ов.
// ============================================================================
#ifndef AOS_X86_64_UNIX_SOCKET_H
#define AOS_X86_64_UNIX_SOCKET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define USOCK_MAX          16
#define USOCK_NAME_LEN     32
#define USOCK_BUF_SIZE     2048
#define USOCK_INVALID_FD   -1

typedef enum {
    USOCK_FREE = 0,
    USOCK_LISTENER,
    USOCK_ENDPOINT,
} usock_type_t;

void   usock_init(void);
int    usock_bind(const char* name);
int    usock_listen(int fd);
int    usock_accept(int fd);
int    usock_connect(const char* name);
size_t usock_send(int fd, const void* data, size_t len);
size_t usock_recv(int fd, void* buf, size_t cap);
void   usock_close(int fd);
bool   usock_has_data(int fd);

#endif

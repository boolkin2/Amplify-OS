// ============================================================================
// unix_socket.c — Реализация Unix Domain Sockets
// ============================================================================

#include "unix_socket.h"
#include "pipe.h"
#include "../memory/heap.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

// Максимальная глубина очереди ожидающих accept()
#define USOCK_BACKLOG  4

typedef struct usock {
    usock_type_t type;

    // --- LISTENER-поля ---
    char name[USOCK_NAME_LEN];
    // Очередь входящих: fd'ы клиентских ENDPOINT'ов, которые ждут accept
    int  backlog[USOCK_BACKLOG];
    int  backlog_count;

    // --- ENDPOINT-поля ---
    // tx — куда мы пишем (наш исходящий поток → peer)
    // rx — откуда мы читаем (входящий поток от peer)
    pipe_t* tx;
    pipe_t* rx;
    int     peer_fd;            // fd противоположного ENDPOINT'а
} usock_t;

static usock_t g_sockets[USOCK_MAX];

// --- Мини-хелперы ---

static int s_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

static void s_strncpy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int s_alloc_slot(void) {
    for (int i = 0; i < USOCK_MAX; i++) {
        if (g_sockets[i].type == USOCK_FREE) return i;
    }
    return USOCK_INVALID_FD;
}

static int s_find_listener(const char* name) {
    for (int i = 0; i < USOCK_MAX; i++) {
        if (g_sockets[i].type == USOCK_LISTENER &&
            s_strcmp(g_sockets[i].name, name) == 0) {
            return i;
        }
    }
    return USOCK_INVALID_FD;
}

// --- API ---

void usock_init(void) {
    for (int i = 0; i < USOCK_MAX; i++) {
        g_sockets[i].type = USOCK_FREE;
        g_sockets[i].backlog_count = 0;
        g_sockets[i].tx = NULL;
        g_sockets[i].rx = NULL;
        g_sockets[i].peer_fd = USOCK_INVALID_FD;
        g_sockets[i].name[0] = '\0';
    }
}

int usock_bind(const char* name) {
    if (!name || !name[0]) return USOCK_INVALID_FD;
    // Имя уже занято?
    if (s_find_listener(name) != USOCK_INVALID_FD) return USOCK_INVALID_FD;

    int fd = s_alloc_slot();
    if (fd < 0) return USOCK_INVALID_FD;

    g_sockets[fd].type = USOCK_LISTENER;
    s_strncpy(g_sockets[fd].name, name, USOCK_NAME_LEN);
    g_sockets[fd].backlog_count = 0;
    return fd;
}

int usock_listen(int fd) {
    if (fd < 0 || fd >= USOCK_MAX) return -1;
    if (g_sockets[fd].type != USOCK_LISTENER) return -1;
    return 0; // В нашей реализации bind сразу переводит в listen-состояние
}

int usock_accept(int fd) {
    if (fd < 0 || fd >= USOCK_MAX) return USOCK_INVALID_FD;
    usock_t* l = &g_sockets[fd];
    if (l->type != USOCK_LISTENER) return USOCK_INVALID_FD;
    if (l->backlog_count == 0) return USOCK_INVALID_FD;

    // FIFO из очереди
    int client_endpoint = l->backlog[0];
    for (int i = 1; i < l->backlog_count; i++) {
        l->backlog[i - 1] = l->backlog[i];
    }
    l->backlog_count--;

    // Создаём наш ("серверный") endpoint, подключённый к клиенту
    int server_fd = s_alloc_slot();
    if (server_fd < 0) {
        // Не смогли — закрываем клиента тоже
        usock_close(client_endpoint);
        return USOCK_INVALID_FD;
    }

    usock_t* srv = &g_sockets[server_fd];
    usock_t* cli = &g_sockets[client_endpoint];

    srv->type    = USOCK_ENDPOINT;
    srv->peer_fd = client_endpoint;
    // Server tx = Client rx; Server rx = Client tx.
    srv->tx = cli->rx;
    srv->rx = cli->tx;

    cli->peer_fd = server_fd;

    return server_fd;
}

int usock_connect(const char* name) {
    if (!name || !name[0]) return USOCK_INVALID_FD;
    int listener_fd = s_find_listener(name);
    if (listener_fd < 0) return USOCK_INVALID_FD;

    usock_t* l = &g_sockets[listener_fd];
    if (l->backlog_count >= USOCK_BACKLOG) return USOCK_INVALID_FD;

    int client_fd = s_alloc_slot();
    if (client_fd < 0) return USOCK_INVALID_FD;

    // Два pipe-а: по одному на направление. Клиент «владеет» обоими
    // до accept(), затем сервер разделит их с ним.
    pipe_t* pa = pipe_create(USOCK_BUF_SIZE);
    pipe_t* pb = pipe_create(USOCK_BUF_SIZE);
    if (!pa || !pb) {
        if (pa) pipe_destroy(pa);
        if (pb) pipe_destroy(pb);
        g_sockets[client_fd].type = USOCK_FREE;
        return USOCK_INVALID_FD;
    }

    usock_t* cli = &g_sockets[client_fd];
    cli->type    = USOCK_ENDPOINT;
    cli->tx      = pa;    // клиент пишет сюда → сервер читает
    cli->rx      = pb;    // сервер пишет сюда → клиент читает
    cli->peer_fd = USOCK_INVALID_FD;

    l->backlog[l->backlog_count++] = client_fd;
    return client_fd;
}

size_t usock_send(int fd, const void* data, size_t len) {
    if (fd < 0 || fd >= USOCK_MAX) return 0;
    usock_t* s = &g_sockets[fd];
    if (s->type != USOCK_ENDPOINT || !s->tx) return 0;
    return pipe_write(s->tx, data, len);
}

size_t usock_recv(int fd, void* buf, size_t cap) {
    if (fd < 0 || fd >= USOCK_MAX) return 0;
    usock_t* s = &g_sockets[fd];
    if (s->type != USOCK_ENDPOINT || !s->rx) return 0;
    return pipe_read(s->rx, buf, cap);
}

bool usock_has_data(int fd) {
    if (fd < 0 || fd >= USOCK_MAX) return false;
    usock_t* s = &g_sockets[fd];
    if (s->type != USOCK_ENDPOINT || !s->rx) return false;
    return pipe_available(s->rx) > 0;
}

void usock_close(int fd) {
    if (fd < 0 || fd >= USOCK_MAX) return;
    usock_t* s = &g_sockets[fd];
    if (s->type == USOCK_FREE) return;

    if (s->type == USOCK_ENDPOINT) {
        // Уведомляем пира EOF на его входе (это наш tx)
        if (s->tx) pipe_close_write(s->tx);
        // Освобождаем «свои» pipe-ы только если пир уже закрыт
        // (чтобы обе стороны могли дочитать) — в MVP освободим оба.
        if (s->peer_fd >= 0 && s->peer_fd < USOCK_MAX &&
            g_sockets[s->peer_fd].type != USOCK_FREE) {
            // Разрываем ссылки у пира
            g_sockets[s->peer_fd].peer_fd = USOCK_INVALID_FD;
        } else {
            // Пир уже исчез — можно удалить pipe-ы
            if (s->tx) pipe_destroy(s->tx);
            if (s->rx) pipe_destroy(s->rx);
        }
        s->tx = NULL;
        s->rx = NULL;
    }
    // LISTENER'ы закрываем просто обнулением
    s->type = USOCK_FREE;
    s->peer_fd = USOCK_INVALID_FD;
    s->backlog_count = 0;
    s->name[0] = '\0';
}

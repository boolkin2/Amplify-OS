// ============================================================================
// pipe.h — Unix-like pipe (Этап 29: IPC)
// ----------------------------------------------------------------------------
// Кольцевой FIFO-буфер фиксированного размера. Используется Shell'ом для
// соединения команд через оператор "|":
//
//     spill /disk/log.txt | sift "error"
//
// Первая команда пишет stdout в pipe (через pipe_write), вторая команда
// читает stdin из того же pipe (через pipe_read).
//
// В kernel без настоящего scheduler'a это sequential: cmd1 выполняется до
// EOF, потом cmd2 читает накопленное. Для демо-пайплайна этого достаточно.
// Будет работать и после внедрения полноценной многозадачности — API
// не меняется.
//
// API:
//   pipe_t* pipe_create(uint32_t capacity) — выделяет pipe с указанной
//                                             ёмкостью (обычно 4–8 KB).
//   size_t pipe_write(p, buf, len)         — записывает как можно больше
//                                             (возврат = сколько влезло).
//   size_t pipe_read(p, buf, cap)          — читает до cap байт.
//   size_t pipe_available(p)               — сколько байт доступно для чтения.
//   bool   pipe_is_full(p)
//   void   pipe_close_write(p)             — помечает конец писательского
//                                             конца (EOF для читателя).
//   bool   pipe_eof(p)                     — writer закрыт и буфер пуст.
//   void   pipe_destroy(p)                 — освобождение памяти.
// ============================================================================

#ifndef PIPE_H
#define PIPE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Максимальный размер буфера по умолчанию
#define PIPE_DEFAULT_CAPACITY  (8 * 1024)

typedef struct pipe {
    uint8_t* buffer;      // Кольцевой буфер данных
    uint32_t capacity;    // Ёмкость буфера
    uint32_t head;        // Позиция чтения (tail для writer'а)
    uint32_t tail;        // Позиция записи
    uint32_t count;       // Текущее количество байт в буфере
    bool     write_closed; // Writer закрыл pipe (EOF)
} pipe_t;

// Создать новый pipe с указанной ёмкостью (0 = PIPE_DEFAULT_CAPACITY).
pipe_t* pipe_create(uint32_t capacity);

// Освободить pipe.
void pipe_destroy(pipe_t* p);

// Записать данные в pipe. Возвращает число реально записанных байт
// (может быть < len, если pipe переполнен).
size_t pipe_write(pipe_t* p, const void* buf, size_t len);

// Прочитать данные из pipe. Возвращает число прочитанных байт
// (0, если пусто, в т.ч. EOF).
size_t pipe_read(pipe_t* p, void* buf, size_t cap);

// Сколько байт доступно для чтения прямо сейчас.
size_t pipe_available(const pipe_t* p);

// Pipe переполнен?
bool pipe_is_full(const pipe_t* p);

// Закрыть писательский конец — больше писать нельзя, читатель после
// опустошения получит EOF.
void pipe_close_write(pipe_t* p);

// EOF: write_closed && буфер пуст.
bool pipe_eof(const pipe_t* p);

#endif // PIPE_H

// ============================================================================
// inflate.h — распаковка DEFLATE / gzip / zlib (подэтап 48.2, «интернет до конца»).
// ----------------------------------------------------------------------------
// Зачем: мы просим серверы отдавать страницы без сжатия (`Accept-Encoding:
// identity`), но часть сайтов и CDN сжимает ответ всё равно — без распаковки
// браузер показывал бы двоичный мусор. Кроме того, со сжатием страница идёт
// по сети в 3–5 раз короче.
// Распаковщик — «весь вход -> весь выход» (окном служит сам выходной буфер),
// без таблиц ускорения: побитовый разбор канонических кодов Хаффмана. 2 МБ
// текста — десятки миллисекунд, для страницы этого достаточно.
// ============================================================================
#ifndef AOS_X86_64_INFLATE_H
#define AOS_X86_64_INFLATE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    INFLATE_OK = 0,          // поток распакован целиком
    INFLATE_TRUNCATED,       // выходной буфер кончился раньше потока — в out начало
    INFLATE_ERR_DATA,        // поток повреждён или обрезан
    INFLATE_ERR_FORMAT,      // это не gzip/zlib/deflate
} inflate_result_t;

// «Сырой» DEFLATE (RFC 1951).
inflate_result_t inflate_raw(const uint8_t* in, size_t in_len,
                             uint8_t* out, size_t out_cap, size_t* out_len);

// Content-Encoding: gzip (RFC 1952) или deflate (zlib RFC 1950 либо сырой
// поток — серверы шлют и так, и так). Формат определяется по заголовку.
inflate_result_t inflate_http(const uint8_t* in, size_t in_len,
                              uint8_t* out, size_t out_cap, size_t* out_len);

#endif

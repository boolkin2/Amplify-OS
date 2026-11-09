// ============================================================================
// stdio_impl.h — внутренняя структура FILE и список открытых потоков.
// ============================================================================
#ifndef _AOS_STDIO_IMPL_H
#define _AOS_STDIO_IMPL_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#define AOS_BUFSZ 1024

typedef struct _AOS_FILE {
    int     fd;                 // POSIX-fd. -1 если закрыт.
    int     flags;              // битмаска, см. ниже
    int     err;                // ferror
    int     eof;                // feof
    int     bufmode;            // _IOFBF/_IOLBF/_IONBF
    unsigned char* buf;         // буфер (NULL если _IONBF)
    size_t  bufsize;
    size_t  bufpos;             // позиция записи/чтения в буфере
    size_t  bufavail;           // сколько байт valid (для read)
    int     own_buf;            // 1 если buf выделен через malloc
    int     pushback;           // этап 40: ungetc(c, stream), -1 если пусто
    struct _AOS_FILE* next;     // глобальный список открытых потоков
} FILE_impl;

#define AOS_FF_READ        0x01
#define AOS_FF_WRITE       0x02
#define AOS_FF_APPEND      0x04

extern FILE_impl* __aos_open_files;

void __aos_register_file  (FILE_impl* f);
void __aos_unregister_file(FILE_impl* f);
int  __aos_flush_one      (FILE_impl* f);
int  __aos_vfprintf       (FILE_impl* f, const char* fmt, va_list ap);

#endif

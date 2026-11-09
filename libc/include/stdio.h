#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>

typedef struct {
    int fd;
} FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

int printf(const char* format, ...);
FILE* fopen(const char* path, const char* mode);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int puts(const char* s);
int putchar(int c);

#endif

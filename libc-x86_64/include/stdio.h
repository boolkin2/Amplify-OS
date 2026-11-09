// ============================================================================
// stdio.h — стандартный ввод-вывод.
// ----------------------------------------------------------------------------
// FILE — непрозрачная структура, определена в src/stdio/stdio_impl.h. Только
// stdin/stdout/stderr раскрыты пользователю.
// ============================================================================
#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _AOS_FILE FILE;

#define EOF             (-1)
#define BUFSIZ          1024
#define FILENAME_MAX    256
#define FOPEN_MAX       32
#define TMP_MAX         32
#define L_tmpnam        20

#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

#define _IOFBF          0   // full buffer
#define _IOLBF          1   // line buffer
#define _IONBF          2   // no buffer

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

// printf-семейство
int  printf  (const char* __restrict__ fmt, ...) __attribute__((format(printf, 1, 2)));
int  fprintf (FILE* __restrict__ s, const char* __restrict__ fmt, ...) __attribute__((format(printf, 2, 3)));
int  sprintf (char* __restrict__ buf, const char* __restrict__ fmt, ...) __attribute__((format(printf, 2, 3)));
int  snprintf(char* __restrict__ buf, size_t n, const char* __restrict__ fmt, ...) __attribute__((format(printf, 3, 4)));
int  vprintf (const char* __restrict__ fmt, va_list ap);
int  vfprintf(FILE* __restrict__ s, const char* __restrict__ fmt, va_list ap);
int  vsprintf(char* __restrict__ buf, const char* __restrict__ fmt, va_list ap);
int  vsnprintf(char* __restrict__ buf, size_t n, const char* __restrict__ fmt, va_list ap);

// scanf-семейство (минимум)
int  sscanf  (const char* __restrict__ s, const char* __restrict__ fmt, ...);
int  vsscanf (const char* __restrict__ s, const char* __restrict__ fmt, va_list ap);

// FILE
FILE* fopen   (const char* __restrict__ path, const char* __restrict__ mode);
FILE* fdopen  (int fd, const char* mode);
FILE* freopen (const char* path, const char* mode, FILE* s);
int   fclose  (FILE* s);
int   fflush  (FILE* s);

size_t fread  (void* __restrict__ buf, size_t sz, size_t n, FILE* __restrict__ s);
size_t fwrite (const void* __restrict__ buf, size_t sz, size_t n, FILE* __restrict__ s);

int   fseek   (FILE* s, long off, int whence);
long  ftell   (FILE* s);
void  rewind  (FILE* s);
int   fgetpos (FILE* s, off_t* pos);
int   fsetpos (FILE* s, const off_t* pos);

int   fgetc   (FILE* s);
int   getc    (FILE* s);
int   getchar (void);
int   ungetc  (int c, FILE* s);

int   fputc   (int c, FILE* s);
int   putc    (int c, FILE* s);
int   putchar (int c);

char* fgets   (char* __restrict__ buf, int n, FILE* __restrict__ s);
int   fputs   (const char* __restrict__ s, FILE* __restrict__ f);
int   puts    (const char* s);

int   feof    (FILE* s);
int   ferror  (FILE* s);
void  clearerr(FILE* s);
int   fileno  (FILE* s);

void  setbuf  (FILE* __restrict__ s, char* __restrict__ buf);
int   setvbuf (FILE* __restrict__ s, char* __restrict__ buf, int mode, size_t size);

int   remove  (const char* path);
int   rename  (const char* old_, const char* new_);
void  perror  (const char* s);

#ifdef __cplusplus
}
#endif
#endif

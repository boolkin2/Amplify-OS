// ============================================================================
// stream.c — FILE*-операции (fopen/fclose/fread/fwrite/fseek/feof/...).
// ============================================================================
#include "stdio_impl.h"
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

void __aos_stdio_flush_all(void);

static int parse_mode(const char* mode, int* flags_out) {
    int flags = 0;
    int o = 0;
    if (!mode || !mode[0]) { errno = EINVAL; return -1; }
    switch (mode[0]) {
    case 'r': o = O_RDONLY; flags |= AOS_FF_READ; break;
    case 'w': o = O_WRONLY | O_CREAT | O_TRUNC; flags |= AOS_FF_WRITE; break;
    case 'a': o = O_WRONLY | O_CREAT | O_APPEND; flags |= AOS_FF_WRITE | AOS_FF_APPEND; break;
    default: errno = EINVAL; return -1;
    }
    if (mode[1] == '+' || (mode[1] == 'b' && mode[2] == '+')) {
        o = (o & ~O_ACCMODE) | O_RDWR;
        flags |= AOS_FF_READ | AOS_FF_WRITE;
    }
    *flags_out = flags;
    return o;
}

FILE* fopen(const char* __restrict__ path, const char* __restrict__ mode) {
    int flags;
    int oflag = parse_mode(mode, &flags);
    if (oflag < 0) return NULL;
    int fd = open(path, oflag, 0666);
    if (fd < 0) return NULL;
    return fdopen(fd, mode);
}

FILE* fdopen(int fd, const char* mode) {
    int flags;
    if (parse_mode(mode, &flags) < 0) return NULL;
    FILE_impl* f = (FILE_impl*)malloc(sizeof(FILE_impl));
    if (!f) return NULL;
    memset(f, 0, sizeof(*f));
    f->fd       = fd;
    f->flags    = flags;
    f->bufmode  = isatty(fd) ? _IOLBF : _IOFBF;
    f->pushback = -1;                    /* этап 40: ungetc pushback пуст */
    f->buf      = (unsigned char*)malloc(AOS_BUFSZ);
    if (!f->buf) { f->bufmode = _IONBF; }
    else         { f->bufsize = AOS_BUFSZ; f->own_buf = 1; }
    __aos_register_file(f);
    return (FILE*)f;
}

int fclose(FILE* stream) {
    FILE_impl* f = (FILE_impl*)stream;
    if (!f) return EOF;
    int rc = __aos_flush_one(f);
    if (f->fd >= 0) close(f->fd);
    __aos_unregister_file(f);
    if (f->own_buf && f->buf) free(f->buf);
    if (f != (FILE_impl*)stdin && f != (FILE_impl*)stdout &&
        f != (FILE_impl*)stderr) {
        free(f);
    }
    return rc;
}

int fflush(FILE* stream) {
    if (!stream) {
        __aos_stdio_flush_all();
        return 0;
    }
    return __aos_flush_one((FILE_impl*)stream);
}

// Чтение: для простоты обходим буфер на каждом вызове (бесполезно для
// большой производительности, но корректно). Когда будут реальные FS-pages,
// перепишем под нормальную буферизацию.
size_t fread(void* __restrict__ buf, size_t sz, size_t n,
             FILE* __restrict__ stream) {
    FILE_impl* f = (FILE_impl*)stream;
    if (!f || !(f->flags & AOS_FF_READ)) { if (f) f->err = 1; return 0; }
    __aos_flush_one(f);                                /* flush write-side */
    size_t total = sz * n;
    size_t got = 0;
    char* p = (char*)buf;
    while (got < total) {
        ssize_t r = read(f->fd, p + got, total - got);
        if (r == 0) { f->eof = 1; break; }
        if (r < 0)  { f->err = 1; break; }
        got += (size_t)r;
    }
    return sz ? (got / sz) : 0;
}

size_t fwrite(const void* __restrict__ buf, size_t sz, size_t n,
              FILE* __restrict__ stream) {
    FILE_impl* f = (FILE_impl*)stream;
    if (!f || !(f->flags & AOS_FF_WRITE)) { if (f) f->err = 1; return 0; }
    size_t total = sz * n;
    const unsigned char* p = (const unsigned char*)buf;

    if (f->bufmode == _IONBF || !f->buf) {
        size_t off = 0;
        while (off < total) {
            ssize_t w = write(f->fd, p + off, total - off);
            if (w <= 0) { f->err = 1; break; }
            off += (size_t)w;
        }
        return sz ? (off / sz) : 0;
    }

    size_t off = 0;
    while (off < total) {
        size_t room = f->bufsize - f->bufpos;
        if (room == 0) {
            if (__aos_flush_one(f) < 0) { f->err = 1; break; }
            room = f->bufsize;
        }
        size_t put = (total - off < room) ? (total - off) : room;
        for (size_t i = 0; i < put; i++) f->buf[f->bufpos + i] = p[off + i];
        f->bufpos += put;
        off += put;

        if (f->bufmode == _IOLBF) {
            for (size_t i = 0; i < put; i++) {
                if (f->buf[f->bufpos - put + i] == '\n') {
                    __aos_flush_one(f);
                    break;
                }
            }
        }
        if (f->bufpos >= f->bufsize) __aos_flush_one(f);
    }
    return sz ? (off / sz) : 0;
}

int fseek(FILE* stream, long off, int whence) {
    FILE_impl* f = (FILE_impl*)stream;
    if (!f) return -1;
    __aos_flush_one(f);
    f->eof = 0;
    return (lseek(f->fd, off, whence) < 0) ? -1 : 0;
}

long ftell(FILE* stream) {
    FILE_impl* f = (FILE_impl*)stream;
    if (!f) return -1L;
    return (long)lseek(f->fd, 0, SEEK_CUR);
}

void rewind(FILE* stream) {
    fseek(stream, 0L, SEEK_SET);
    if (stream) ((FILE_impl*)stream)->err = 0;
}

int feof  (FILE* s) { return s ? ((FILE_impl*)s)->eof : 0; }
int ferror(FILE* s) { return s ? ((FILE_impl*)s)->err : 0; }
void clearerr(FILE* s) { if (s) { ((FILE_impl*)s)->err = 0; ((FILE_impl*)s)->eof = 0; } }
int fileno(FILE* s)   { return s ? ((FILE_impl*)s)->fd : -1; }

int fputc(int c, FILE* s) {
    unsigned char ch = (unsigned char)c;
    return (fwrite(&ch, 1, 1, s) == 1) ? c : EOF;
}
int putc   (int c, FILE* s) { return fputc(c, s); }
int putchar(int c)          { return fputc(c, stdout); }

int fputs(const char* __restrict__ s, FILE* __restrict__ f) {
    size_t n = 0; while (s[n]) n++;
    return (fwrite(s, 1, n, f) == n) ? (int)n : EOF;
}
int puts(const char* s) {
    if (fputs(s, stdout) == EOF) return EOF;
    return fputc('\n', stdout);
}

int fgetc(FILE* s) {
    FILE_impl* f = (FILE_impl*)s;
    if (!f) return EOF;
    // Этап 40: push-back символ имеет приоритет над read().
    if (f->pushback >= 0) {
        int c = f->pushback;
        f->pushback = -1;
        return c;
    }
    unsigned char ch;
    size_t r = fread(&ch, 1, 1, s);
    return r == 1 ? (int)ch : EOF;
}
int getc   (FILE* s) { return fgetc(s); }
int getchar(void)    { return fgetc(stdin); }
int ungetc (int c, FILE* s) {
    FILE_impl* f = (FILE_impl*)s;
    if (!f || c == EOF) return EOF;
    if (f->pushback >= 0) return EOF;   // уже есть push-back — только один
    f->pushback = c & 0xFF;
    f->eof = 0;                          // ungetc сбрасывает EOF
    return c;
}

char* fgets(char* __restrict__ buf, int n, FILE* __restrict__ s) {
    if (n <= 0 || !buf) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = fgetc(s);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    buf[i] = 0;
    return buf;
}

void setbuf(FILE* __restrict__ s, char* __restrict__ buf) {
    setvbuf(s, buf, buf ? _IOFBF : _IONBF, AOS_BUFSZ);
}

int setvbuf(FILE* __restrict__ s, char* __restrict__ buf, int mode, size_t sz) {
    FILE_impl* f = (FILE_impl*)s;
    if (!f) return -1;
    __aos_flush_one(f);
    if (f->own_buf && f->buf) free(f->buf);
    if (mode == _IONBF) {
        f->buf = NULL; f->bufsize = 0; f->own_buf = 0;
    } else if (buf) {
        f->buf = (unsigned char*)buf; f->bufsize = sz; f->own_buf = 0;
    } else {
        f->buf = (unsigned char*)malloc(sz ? sz : AOS_BUFSZ);
        f->bufsize = sz ? sz : AOS_BUFSZ; f->own_buf = 1;
    }
    f->bufmode = mode;
    f->bufpos  = 0;
    return 0;
}

void perror(const char* str) {
    extern char* strerror(int);
    if (str && *str) fputs(str, stderr), fputs(": ", stderr);
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

int remove(const char* path) { (void)path; errno = ENOSYS; return -1; }
int rename(const char* a, const char* b) { (void)a; (void)b; errno = ENOSYS; return -1; }

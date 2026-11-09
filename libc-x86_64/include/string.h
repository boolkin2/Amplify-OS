// ============================================================================
// string.h — POSIX manipulation байт/строк.
// ============================================================================
#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void*  memcpy (void* __restrict__ dst, const void* __restrict__ src, size_t n);
void*  memmove(void* dst, const void* src, size_t n);
void*  memset (void* s, int c, size_t n);
int    memcmp (const void* a, const void* b, size_t n);
void*  memchr (const void* s, int c, size_t n);

size_t strlen (const char* s);
size_t strnlen(const char* s, size_t maxlen);

int    strcmp (const char* a, const char* b);
int    strncmp(const char* a, const char* b, size_t n);
int    strcasecmp (const char* a, const char* b);
int    strncasecmp(const char* a, const char* b, size_t n);

char*  strcpy (char* __restrict__ dst, const char* __restrict__ src);
char*  strncpy(char* __restrict__ dst, const char* __restrict__ src, size_t n);
char*  strcat (char* __restrict__ dst, const char* __restrict__ src);
char*  strncat(char* __restrict__ dst, const char* __restrict__ src, size_t n);

char*  strchr (const char* s, int c);
char*  strrchr(const char* s, int c);
char*  strstr (const char* haystack, const char* needle);

char*  strdup (const char* s);
char*  strndup(const char* s, size_t n);

char*  strtok (char* __restrict__ s, const char* __restrict__ sep);
char*  strtok_r(char* __restrict__ s, const char* __restrict__ sep,
                char** __restrict__ saveptr);

size_t strspn (const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char*  strpbrk(const char* s, const char* accept);

char*  strerror(int errnum);

#ifdef __cplusplus
}
#endif
#endif

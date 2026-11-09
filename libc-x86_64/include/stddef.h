// ============================================================================
// stddef.h — стандартные типы и макросы (musl-aos libc для AMPLIFY-OS x86_64).
// ----------------------------------------------------------------------------
// Заголовок намеренно тонкий: ничего лишнего, чтобы не плодить конфликтов с
// GCC-builtin'ами. NULL — единый, size_t/ptrdiff_t — из stdint.
// ============================================================================
#ifndef _STDDEF_H
#define _STDDEF_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef _SIZE_T_DEFINED
#define _SIZE_T_DEFINED
typedef unsigned long size_t;
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef long ssize_t;
#endif

#ifndef _PTRDIFF_T_DEFINED
#define _PTRDIFF_T_DEFINED
typedef long ptrdiff_t;
#endif

#ifndef _WCHAR_T_DEFINED
#define _WCHAR_T_DEFINED
typedef int wchar_t;
#endif

#define offsetof(t, m) __builtin_offsetof(t, m)

#ifdef __cplusplus
}
#endif
#endif

// ============================================================================
// ctype.c — классификация ASCII-символов. Никаких таблиц локали — на этапе
// 52 (когда придёт UTF-8/locale) расширим.
// ============================================================================
#include <ctype.h>

int isalpha (int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int isdigit (int c) { return c >= '0' && c <= '9'; }
int isalnum (int c) { return isalpha(c) || isdigit(c); }
int isupper (int c) { return c >= 'A' && c <= 'Z'; }
int islower (int c) { return c >= 'a' && c <= 'z'; }
int isspace (int c) { return c == ' ' || (c >= 9 && c <= 13); }
int isblank (int c) { return c == ' ' || c == '\t'; }
int iscntrl (int c) { return (c >= 0 && c < 32) || c == 127; }
int isgraph (int c) { return c > 32 && c < 127; }
int isprint (int c) { return c >= 32 && c < 127; }
int ispunct (int c) { return isgraph(c) && !isalnum(c); }
int isxdigit(int c) { return isdigit(c) ||
                             (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }
int isascii (int c) { return (unsigned)c < 128; }
int toupper (int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }
int tolower (int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
int toascii (int c) { return c & 0x7F; }

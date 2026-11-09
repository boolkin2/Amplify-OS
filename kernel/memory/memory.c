// ============================================================================
// memory.c - Реализация вспомогательных функций для работы с памятью
// ============================================================================
// Быстрый memcpy/memset на i386: используем `rep movsd`/`rep stosd` для
// 32-битных блоков (один микрооп на 4 байта в QEMU вместо одного на 1 байт).
// Разница на 3-MB framebuffer'ах — буквально в десятки раз. Это критично
// для плавности WM-композитинга (save_scene/restore_scene/swap).
// ============================================================================

#include "memory.h"

// ============================================================================
// Функции работы с памятью
// ============================================================================

// Копирование памяти — оптимизированная версия через rep movsd + rep movsb.
void* memcpy(void* dest, const void* src, size_t n) {
    void* ret = dest;
    size_t dwords = n >> 2;            // сколько 4-байтовых блоков
    size_t tail   = n & 3;             // остаток 0..3 байта

    // rep movsd: ESI/EDI инкрементируются автоматически (DF=0).
    __asm__ volatile(
        "cld\n\t"
        "rep movsl\n\t"
        "mov %3, %%ecx\n\t"
        "rep movsb\n\t"
        : "+D"(dest), "+S"(src), "+c"(dwords)
        : "r"(tail)
        : "memory"
    );
    return ret;
}

// Заполнение памяти — оптимизированная версия через rep stosd + rep stosb.
void* memset(void* s, int c, size_t n) {
    void* ret = s;
    uint8_t  b = (uint8_t)c;
    uint32_t pattern = ((uint32_t)b) | ((uint32_t)b << 8)
                     | ((uint32_t)b << 16) | ((uint32_t)b << 24);
    size_t dwords = n >> 2;
    size_t tail   = n & 3;

    __asm__ volatile(
        "cld\n\t"
        "rep stosl\n\t"
        "mov %3, %%ecx\n\t"
        "mov %4, %%eax\n\t"
        "rep stosb\n\t"
        : "+D"(s), "+c"(dwords)
        : "a"(pattern), "r"(tail), "r"((uint32_t)b)
        : "memory"
    );
    return ret;
}

// Сравнение памяти
int memcmp(const void* s1, const void* s2, size_t n) {
    const uint8_t* p1 = (const uint8_t*)s1;
    const uint8_t* p2 = (const uint8_t*)s2;
    
    for (size_t i = 0; i < n; i++) {
        if (p1[i] != p2[i]) {
            return p1[i] - p2[i];
        }
    }
    
    return 0;
}

// Перемещение памяти (с учётом перекрытия)
void* memmove(void* dest, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dest;
    const uint8_t* s = (const uint8_t*)src;
    
    if (d < s) {
        // Копируем вперёд
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else if (d > s) {
        // Копируем назад (чтобы избежать перезаписи)
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    
    return dest;
}

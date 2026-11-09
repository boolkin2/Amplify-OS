// ============================================================================
// multiboot2.c — поиск тэгов в boot info структуре.
// ============================================================================
#include "multiboot2.h"

const mb2_tag_t* mb2_find_tag(const mb2_info_t* info, uint32_t type) {
    if (!info) return 0;

    const uint8_t* p = (const uint8_t*)info + 8;       // пропускаем total_size+reserved
    const uint8_t* end = (const uint8_t*)info + info->total_size;

    while (p < end) {
        const mb2_tag_t* t = (const mb2_tag_t*)p;
        if (t->type == MB2_TAG_END) return 0;
        if (t->type == type) return t;

        // Каждый тэг выровнен по 8 байтам.
        uint32_t step = (t->size + 7) & ~7u;
        p += step;
    }
    return 0;
}

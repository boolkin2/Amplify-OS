// ============================================================================
// ui_icons.h — растровые иконки дизайн-системы AMPLIFY-OS "Pearl".
// ----------------------------------------------------------------------------
// 48x48 ARGB-иконки приложений + логотип AMPLIFY (большой и малый).
// ============================================================================
#ifndef AOS_X86_64_UI_ICONS_H
#define AOS_X86_64_UI_ICONS_H

#include <stdint.h>
#include "graphics.h"

#define UI_ICON_SIZE 48

typedef enum {
    UI_ICON_AMPLIFY,
    UI_ICON_FILES,
    UI_ICON_FOLDER,
    UI_ICON_BROWSER,
    UI_ICON_MAIL,
    UI_ICON_TERMINAL,
    UI_ICON_SETTINGS,
    UI_ICON_GALLERY,
    UI_ICON_MUSIC,
    UI_ICON_CALENDAR,
    UI_ICON_NOTES,
    UI_ICON_STORE,
    UI_ICON_TRASH,
    UI_ICON_RUNE,       // [51.4] Rune: данные в ui_icon_rune.c (tools/gen_rune_icon.py)
    UI_ICON_COUNT
} ui_icon_id_t;

typedef struct {
    const uint32_t* px;
    int w;
    int h;
} ui_bitmap_t;

const ui_bitmap_t* ui_icon(ui_icon_id_t id);
ui_icon_id_t ui_icon_by_name(const char* name);
const ui_bitmap_t* ui_logo_big(void);
const ui_bitmap_t* ui_logo_small(void);

void ui_draw_bitmap(const gfx_target_t* t, int x, int y, const ui_bitmap_t* bmp);
void ui_draw_bitmap_scaled(const gfx_target_t* t, int x, int y,
                           int dst_w, int dst_h, const ui_bitmap_t* bmp);
void ui_draw_bitmap_alpha(const gfx_target_t* t, int x, int y,
                          int dst_w, int dst_h, const ui_bitmap_t* bmp,
                          uint8_t alpha);

#endif

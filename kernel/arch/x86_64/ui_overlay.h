#ifndef AOS_X86_64_UI_OVERLAY_H
#define AOS_X86_64_UI_OVERLAY_H

#include "graphics.h"
#include <stdbool.h>

typedef enum { UI_OVERLAY_NONE=0, UI_OVERLAY_DRAWER, UI_OVERLAY_MODAL, UI_OVERLAY_TOAST } ui_overlay_kind_t;

void ui_overlay_init(void);
void ui_overlay_show_drawer(const char* title, const char* body);
void ui_overlay_show_modal(const char* title, const char* body);
void ui_overlay_show_toast(const char* text, unsigned duration_ms);
void ui_overlay_dismiss(void);
bool ui_overlay_is_open(void);
bool ui_overlay_visible(void);      // [48.2] открыт или ещё исчезает
bool ui_overlay_motion_active(void);
void ui_overlay_draw(const gfx_target_t* t);
bool ui_overlay_handle_click(int x, int y);
bool ui_overlay_handle_key(char c);

#endif

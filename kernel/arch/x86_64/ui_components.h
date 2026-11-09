#ifndef AOS_X86_64_UI_COMPONENTS_H
#define AOS_X86_64_UI_COMPONENTS_H

#include "graphics.h"
#include "ui_icons.h"
#include "ui_theme.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { UI_CONTROL_IDLE, UI_CONTROL_HOVER, UI_CONTROL_PRESSED, UI_CONTROL_DISABLED } ui_control_state_t;

void ui_draw_card(const gfx_target_t* t, int x, int y, int w, int h, int radius);
void ui_draw_section_label(const gfx_target_t* t, int x, int y, const char* text);
void ui_draw_button(const gfx_target_t* t, int x, int y, int w, int h,
                    const char* label, bool primary, ui_control_state_t state);
void ui_draw_metric(const gfx_target_t* t, int x, int y, int w, int h,
                    const char* value, const char* label, uint8_t progress);
void ui_draw_ring(const gfx_target_t* t, int cx, int cy, int radius, int thickness,
                  uint8_t progress, uint32_t track, uint32_t accent);
void ui_draw_app_shell(const gfx_target_t* t, int active_slot, int hover_slot,
                       const ui_icon_id_t* icons, int icon_count);

// Этап F: true, пока подсветка рейла ещё едет — WM держит 50 FPS.
bool ui_components_motion_active(void);

#endif
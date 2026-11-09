#ifndef AOS_X86_64_BEACON_H
#define AOS_X86_64_BEACON_H
#include "graphics.h"
#include <stdbool.h>
void beacon_init(void);
void beacon_show(void);
void beacon_dismiss(void);
bool beacon_is_open(void);
bool beacon_visible(void);
bool beacon_motion_active(void);
void beacon_draw(const gfx_target_t* target);
bool beacon_handle_click(int screen_x, int screen_y);
bool beacon_handle_key(char key);
#endif

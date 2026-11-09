#ifndef AOS_X86_64_CONSTELLATION_H
#define AOS_X86_64_CONSTELLATION_H
#include "graphics.h"
#include <stdbool.h>
void constellation_init(void);
void constellation_show(void);
void constellation_dismiss(void);
bool constellation_is_open(void);
bool constellation_visible(void);
bool constellation_motion_active(void);
void constellation_draw(const gfx_target_t* target);
bool constellation_handle_click(int screen_x, int screen_y);
bool constellation_handle_key(char key);
struct window;
// Этап F: убрать уничтоженное окно из списка переключателя.
void constellation_forget_window(struct window* w);
#endif
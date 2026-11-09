#ifndef AOS_X86_64_UI_LAYOUT_H
#define AOS_X86_64_UI_LAYOUT_H

#include <stdbool.h>

#define UI_LOGICAL_W 1600
#define UI_LOGICAL_H 1000

typedef struct {
    int screen_w;
    int screen_h;
    int scale_fp;   /* 16.16, aspect-preserving */
    int offset_x;
    int offset_y;
} ui_layout_t;

void ui_layout_update(int screen_w, int screen_h);
const ui_layout_t* ui_layout_get(void);
int ui_ux(int logical_x);
int ui_uy(int logical_y);
int ui_uw(int logical_w);
int ui_uh(int logical_h);
int ui_lx(int screen_x);
int ui_ly(int screen_y);
bool ui_hit_logical(int screen_x, int screen_y, int x, int y, int w, int h);

#endif

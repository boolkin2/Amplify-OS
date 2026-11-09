#ifndef AOS_X86_64_APP_STAGE_E_H
#define AOS_X86_64_APP_STAGE_E_H
#include "window.h"
#include "graphics.h"
#include "ui_theme.h"
#include "ui_components.h"
#include "ui_icons.h"
#include <stdint.h>
#include <stdbool.h>
void se_text(const gfx_target_t*,int,int,const char*,uint32_t);
void se_bold(const gfx_target_t*,int,int,const char*,uint32_t);
void se_card(const gfx_target_t*,int,int,int,int,uint32_t,uint32_t);
void se_num(char*,uint64_t);
void se_copy(char*,const char*,int);
int se_len(const char*);
bool se_contains(const char*,const char*);
window_t* se_workspace(const char*,window_draw_fn,window_click_fn,window_key_fn,void*,bool);
void se_close(window_t*);
void se_header(const gfx_target_t*,const char*,const char*,bool);
void se_progress(const gfx_target_t*,int,int,int,uint8_t,uint32_t,uint32_t);
void se_ip(char out[16],uint32_t ip);
#endif

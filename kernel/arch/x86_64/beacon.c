// AMPLIFY-OS Pearl Stage C — Beacon notification center.
#include "beacon.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_components.h"
#include "ui_icons.h"
#include "ui_motion.h"
#include "ui_fx.h"
#include "timer.h"
#include "keyboard.h"
#include <stdint.h>

typedef struct {
    const char* group;
    const char* title;
    const char* detail;
    ui_icon_id_t icon;
    bool unread;
    bool visible;
} beacon_item_t;

static beacon_item_t g_items[] = {
    {"THREADS", "Ring rotation decision", "CHRT spec - anchored 10:31", UI_ICON_NOTES, true, true},
    {"THREADS", "Prism team - Ring rotation", "Needs you - 2 attachments", UI_ICON_MAIL, true, true},
    {"FACETS & STRATA", "Ring motion", "48 frames - warm layer", UI_ICON_GALLERY, false, true},
    {"FACETS & STRATA", "ring-motion.mov", "84 mb - warm layer", UI_ICON_FILES, false, true},
    {"DO IT NOW", "Ask Pulse to test both rotations", "Writes the plan, asks once", UI_ICON_TERMINAL, true, true}
};
#define BEACON_COUNT ((int)(sizeof(g_items)/sizeof(g_items[0])))

// [48.2] Панель выезжает справа и проявляется из снимка (ui_fx.h), уезжает
// обратно при закрытии; экран под ней приглушается.
static fx_panel_t g_fx = { .name = "beacon" };
#define g_open (g_fx.shown)
static int g_selected;

static void text(const gfx_target_t*t,int x,int y,const char*s,uint32_t c){gfx_draw_string_transparent(t,ui_ux(x),ui_uy(y),s,c);}
static void bold(const gfx_target_t*t,int x,int y,const char*s,uint32_t c){text(t,x,y,s,c);gfx_draw_string_transparent(t,ui_ux(x)+1,ui_uy(y),s,c);}
static void fill(const gfx_target_t*t,int x,int y,int w,int h,uint32_t c){gfx_fill_rect(t,ui_ux(x),ui_uy(y),ui_uw(w),ui_uh(h),c);}
static void round_fill(const gfx_target_t*t,int x,int y,int w,int h,int r,uint32_t c){gfx_fill_round_rect(t,ui_ux(x),ui_uy(y),ui_uw(w),ui_uh(h),ui_uw(r),c);}
static int first_visible(void){for(int i=0;i<BEACON_COUNT;i++)if(g_items[i].visible)return i;return 0;}
static int visible_count(void){int n=0;for(int i=0;i<BEACON_COUNT;i++)if(g_items[i].visible)n++;return n;}

void beacon_init(void){g_fx.shown=false;g_fx.closing=false;g_selected=0;}
void beacon_show(void){g_selected=first_visible();fx_panel_show(&g_fx,320);}
void beacon_dismiss(void){fx_panel_hide(&g_fx,220);}
bool beacon_is_open(void){return g_open;}
bool beacon_visible(void){return fx_panel_visible(&g_fx);}
bool beacon_motion_active(void){return fx_panel_moving(&g_fx);}

static void draw_row(const gfx_target_t*t,int idx,int x,int y,int w){
    const ui_runtime_theme_t*th=ui_theme_current();beacon_item_t*it=&g_items[idx];
    if(idx==g_selected){round_fill(t,x-8,y-8,w+16,66,10,th->surface_soft);fill(t,x-8,y-8,3,66,th->accent_coral);}
    round_fill(t,x,y,42,42,10,it->unread?th->accent_coral:th->surface_soft);
    ui_draw_bitmap_scaled(t,ui_ux(x+10),ui_uy(y+10),ui_uw(22),ui_uh(22),ui_icon(it->icon));
    bold(t,x+58,y+2,it->title,th->ink);text(t,x+58,y+25,it->detail,th->muted);
    if(it->unread)round_fill(t,x+w-14,y+12,7,7,4,th->accent_berry);
}

static void beacon_panel(const gfx_target_t*t,void*arg);
void beacon_draw(const gfx_target_t*t){
    if(!t||!fx_panel_visible(&g_fx))return;
    ui_layout_update(t->width,t->height);
    fx_panel_frame(&g_fx,t,0x241F27,130,ui_ux(430),ui_uy(150),ui_uw(760),ui_uh(608),ui_uw(18),
                   ui_uw(220),0,beacon_panel,0);
}
static void beacon_panel(const gfx_target_t*t,void*arg){
    (void)arg;
    const ui_runtime_theme_t*th=ui_theme_current();
    int x=430,y=150,w=760,h=608;
    round_fill(t,x,y,w,h,18,th->border);round_fill(t,x+1,y+1,w-2,h-2,17,th->surface);
    round_fill(t,x+34,y+34,14,14,7,th->accent_coral);bold(t,x+68,y+28,"ring rotation",th->ink);
    fill(t,x+230,y+25,2,28,th->accent_berry);
    ui_draw_button(t,x+w-114,y+24,82,30,"Clear",false,UI_CONTROL_IDLE);
    fill(t,x,y+86,w,1,th->border);
    if(visible_count()==0){bold(t,x+34,y+132,"Beacon is quiet",th->ink);text(t,x+34,y+160,"Nothing needs your attention.",th->muted);return;}
    int row_y=y+138;const char*last=0;
    for(int i=0;i<BEACON_COUNT;i++)if(g_items[i].visible){
        if(last!=g_items[i].group){text(t,x+34,row_y,g_items[i].group,th->muted);fill(t,x+190,row_y+7,w-224,1,th->border);row_y+=30;last=g_items[i].group;}
        draw_row(t,i,x+46,row_y,w-92);row_y+=76;
    }
    text(t,x+34,y+h-28,"Up/Down select  Enter read  Delete dismiss  Esc close",th->muted);
}

static int row_at(int lx,int ly){
    (void)lx;int y=288;const char*last=0;
    for(int i=0;i<BEACON_COUNT;i++)if(g_items[i].visible){if(last!=g_items[i].group){y+=30;last=g_items[i].group;}if(ly>=y&&ly<y+66)return i;y+=76;}
    return -1;
}
bool beacon_handle_click(int sx,int sy){
    if(!g_open)return false;
    int x=ui_lx(sx),y=ui_ly(sy);
    if(x<430||x>1190||y<150||y>758){beacon_dismiss();return true;}
    if(x>=1076&&x<1158&&y>=174&&y<204){for(int i=0;i<BEACON_COUNT;i++)g_items[i].visible=false;return true;}
    int idx=row_at(x,y);if(idx>=0){g_selected=idx;g_items[idx].unread=false;}return true;
}
bool beacon_handle_key(char c){
    if(!g_open)return false;
    if(c==27){beacon_dismiss();return true;}
    if(c=='c'||c=='C'){for(int i=0;i<BEACON_COUNT;i++)g_items[i].visible=false;return true;}
    if(c=='d'||c=='D'||c==127){g_items[g_selected].visible=false;g_selected=first_visible();return true;}
    if(c=='\n'||c==' '){g_items[g_selected].unread=false;return true;}
    int dir=(c==KBD_KEY_UP||c==KBD_KEY_LEFT)?-1:(c==KBD_KEY_DOWN||c==KBD_KEY_RIGHT||c=='\t')?1:0;
    if(dir){int n=g_selected;for(int k=0;k<BEACON_COUNT;k++){n=(n+dir+BEACON_COUNT)%BEACON_COUNT;if(g_items[n].visible){g_selected=n;break;}}return true;}
    return true;
}

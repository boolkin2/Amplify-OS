// AMPLIFY-OS Pearl Stage C — Constellation live window switcher.
#include "constellation.h"
#include "wm.h"
#include "window.h"
#include "keyboard.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "ui_icons.h"
#include "ui_motion.h"
#include "ui_fx.h"
#include <stdint.h>

#define CONST_MAX 8
// [48.2] Слой переключателя целиком (вуаль + карточки) проявляется из снимка
// экрана, слегка «наезжая» на зрителя; при закрытии — обратно (ui_fx.h).
static fx_panel_t g_fx = { .name = "constellation" };
#define g_open (g_fx.shown)
static window_t* g_windows[CONST_MAX];
static int g_count,g_selected;

static void text(const gfx_target_t*t,int x,int y,const char*s,uint32_t c){gfx_draw_string_transparent(t,ui_ux(x),ui_uy(y),s,c);}
static void bold(const gfx_target_t*t,int x,int y,const char*s,uint32_t c){text(t,x,y,s,c);gfx_draw_string_transparent(t,ui_ux(x)+1,ui_uy(y),s,c);}
static void round_fill(const gfx_target_t*t,int x,int y,int w,int h,int r,uint32_t c){gfx_fill_round_rect(t,ui_ux(x),ui_uy(y),ui_uw(w),ui_uh(h),ui_uw(r),c);}

void constellation_init(void){g_fx.shown=false;g_fx.closing=false;g_count=0;g_selected=0;}
static void collect(void){g_count=0;window_t*f=wm_focused();for(window_t*w=wm_iterate(0);w&&g_count<CONST_MAX;w=wm_iterate(w))if(w->visible){g_windows[g_count]=w;if(w==f)g_selected=g_count;g_count++;}if(g_selected>=g_count)g_selected=0;}
void constellation_show(void){collect();if(g_count==0)return;fx_panel_show(&g_fx,300);}
void constellation_dismiss(void){fx_panel_hide(&g_fx,200);}
bool constellation_is_open(void){return g_open;}
bool constellation_visible(void){return fx_panel_visible(&g_fx);}
bool constellation_motion_active(void){return fx_panel_moving(&g_fx);}

static void thumbnail(const gfx_target_t*t,window_t*w,int x,int y,int dw,int dh,bool selected){
    const ui_runtime_theme_t*th=ui_theme_current();uint32_t border=selected?th->accent_coral:th->border;
    round_fill(t,x-3,y-3,dw+6,dh+6,16,border);round_fill(t,x,y,dw,dh,14,th->surface);
    if(!w||!w->buffer||w->width<=0||w->height<=0)return;
    int dx=ui_ux(x),dy=ui_uy(y),pw=ui_uw(dw),ph=ui_uh(dh);
    for(int yy=8;yy<ph-8;yy++){
        int sy=((yy-8)*w->height)/(ph>16?ph-16:1);uint32_t*dst=(uint32_t*)((uint8_t*)t->pixels+(dy+yy)*t->pitch)+dx;
        for(int xx=8;xx<pw-8;xx++){int sx=((xx-8)*w->width)/(pw>16?pw-16:1);dst[xx]=w->buffer[sy*w->width+sx];}
    }
}
static void constellation_layer(const gfx_target_t*t,void*arg);
void constellation_draw(const gfx_target_t*t){
    if(!t||!fx_panel_visible(&g_fx))return;
    ui_layout_update(t->width,t->height);
    fx_layer_frame(&g_fx,t,constellation_layer,0);
}
static void constellation_layer(const gfx_target_t*t,void*arg){
    (void)arg;
    const ui_runtime_theme_t*th=ui_theme_current();uint8_t p=255;
    // Вуаль цвета холста: рабочий стол под ней едва угадывается и не спорит
    // с подписями (раньше здесь была сплошная заливка).
    fx_dim(t,0,0,t->width,t->height,th->canvas&0x00FFFFFFu,236);
    text(t,656,112,"CONSTELLATION - ARROWS TO SWITCH",th->muted);
    char count[24]="Open prisms: ";count[13]='0'+(g_count>9?9:g_count);count[14]=0;bold(t,668,142,count,th->ink);
    int cols=g_count<4?g_count:4;int card_w=280,card_h=350,gap=28,total=cols*card_w+(cols-1)*gap,start=(1600-total)/2;
    for(int i=0;i<g_count;i++){
        int row=i/4,col=i%4,x=start+col*(card_w+gap),y=236+row*410;
        int lift=(i==g_selected)?-14:0;int appear=(255-p)*(i+1)*2/5;
        thumbnail(t,g_windows[i],x,y+lift+appear,card_w,card_h,i==g_selected);
        round_fill(t,x+18,y+lift+appear+18,40,40,10,i==g_selected?th->accent_coral:th->surface_soft);
        ui_draw_bitmap_scaled(t,ui_ux(x+28),ui_uy(y+lift+appear+28),ui_uw(20),ui_uh(20),ui_icon(i==0?UI_ICON_TERMINAL:UI_ICON_NOTES));
        bold(t,x+8,y+lift+appear+card_h+18,g_windows[i]->title,th->ink);
        text(t,x+8,y+lift+appear+card_h+42,i==g_selected?"front prism":"open workspace",th->muted);
    }
    text(t,570,900,"Enter brings the selected prism forward - Esc returns",th->muted);
}
bool constellation_handle_click(int sx,int sy){
    if(!g_open)return false;
    int x=ui_lx(sx),y=ui_ly(sy);int cols=g_count<4?g_count:4,card_w=280,gap=28,total=cols*card_w+(cols-1)*gap,start=(1600-total)/2;
    for(int i=0;i<g_count;i++){int cx=start+(i%4)*(card_w+gap),cy=222+(i/4)*410;if(x>=cx&&x<cx+card_w&&y>=cy&&y<cy+390){g_selected=i;wm_focus(g_windows[i]);constellation_dismiss();return true;}}
    constellation_dismiss();return true;
}
bool constellation_handle_key(char c){
    if(!g_open)return false;
    if(c==27){constellation_dismiss();return true;}
    if(c=='\n'||c==' '){if(g_count){wm_focus(g_windows[g_selected]);}constellation_dismiss();return true;}
    if(c==KBD_KEY_LEFT||c==KBD_KEY_UP||c==KBD_KEY_BACKTAB)g_selected=(g_selected+g_count-1)%g_count;
    else if(c==KBD_KEY_RIGHT||c==KBD_KEY_DOWN||c=='\t')g_selected=(g_selected+1)%g_count;
    return true;
}

// Этап F: WM уничтожает закрытые окна, поэтому собранный список
// переключателя не должен держать висячие указатели.
void constellation_forget_window(struct window* w){
    int n=0;
    for(int i=0;i<g_count;i++){if(g_windows[i]==(window_t*)w)continue;g_windows[n++]=g_windows[i];}
    g_count=n;
    if(g_selected>=g_count)g_selected=g_count?g_count-1:0;
}

// AMPLIFY-OS Pearl Stage C — Ignite boot screen.
#include "splash.h"
#include "graphics.h"
#include "doublebuf.h"
#include "fb.h"
#include "timer.h"
#include "speaker.h"
#include "ui_theme.h"
#include "ui_layout.h"
#include "ui_icons.h"
#include "ui_font.h"
#include <stdint.h>

static void text(const gfx_target_t*t,int x,int y,const char*s,uint32_t c){gfx_draw_string_transparent(t,ui_ux(x),ui_uy(y),s,c);}
static void fill(const gfx_target_t*t,int x,int y,int w,int h,uint32_t c){gfx_fill_rect(t,ui_ux(x),ui_uy(y),ui_uw(w),ui_uh(h),c);}
/* Этап F: центруем по реальной ширине глифов, а не по strlen*8. */
static int logical_w(const gfx_target_t*t,int px){return t->width?(px*1600)/t->width:px;}
static void center_tracked(const gfx_target_t*t,int y,const char*s,uint32_t c,ui_font_id_t id,int tracking){
    int wl=logical_w(t,ui_font_text_width_tracked(s,id,tracking));
    ui_font_draw_tracked(t,ui_ux((1600-wl)/2),ui_uy(y),s,c,id,tracking);
}
static void gradient_bar(const gfx_target_t*t,int x,int y,int w,int h,int progress){
    int fw=(w*progress)/100;fill(t,x,y,w,h,0xFF352A36);
    for(int i=0;i<fw;i++){uint8_t q=(uint8_t)((i*255)/(w?w:1));fill(t,x+i,y,1,h,ui_sunset(q));}
}
void splash_show(void){
    const gfx_target_t*t=doublebuf_target();if(!t)t=gfx_screen();if(!t)return;
    ui_layout_update(t->width,t->height);speaker_play_boot_melody();
    static const char*status[4]={"SEEDING MEMORY","WAKING PRISMS","IGNITING CORE","OPENING IDENTITY"};
    const int frames=40;
    for(int frame=0;frame<=frames;frame++){
        int progress=(frame*100)/frames;
        gfx_clear(t,UI_DARK_CANVAS);
        /* The quiet oversized A is the principal Ignite watermark. */
        ui_draw_bitmap_alpha(t,ui_ux(275),ui_uy(-30),ui_uw(1050),ui_uh(1050),ui_logo_big(),20);
        ui_draw_bitmap_scaled(t,ui_ux(735),ui_uy(330),ui_uw(130),ui_uh(130),ui_logo_big());
        center_tracked(t,505,"AMPLIFY-OS",UI_DARK_TEXT,UI_FONT_44,7);
        center_tracked(t,582,"CHRT CORE 1.0",0xFFA99FAA,UI_FONT_12,5);
        fill(t,640,590,320,1,0x55352A36);
        gradient_bar(t,630,655,340,4,progress);
        text(t,630,674,status[(frame*4)/(frames+1)],0xFFA99FAA);
        char pct[5];pct[0]='0'+(progress/100);pct[1]='0'+((progress/10)%10);pct[2]='0'+(progress%10);pct[3]='%';pct[4]=0;
        if(progress<100){pct[0]=progress>=10?'0'+progress/10:' ';pct[1]='0'+progress%10;pct[2]='%';pct[3]=0;}
        text(t,930,674,pct,0xFFA99FAA);
        fill(t,500,729,600,1,0x44352A36);
        text(t,56,938,"build 2026.09 - prism kernel - cold start",0xFFA99FAA);
        text(t,1304,938,"HOLD A FOR RECOVERY STRATA",0xFFA99FAA);
        doublebuf_swap();timer_sleep(38);
    }
    timer_sleep(180);
}

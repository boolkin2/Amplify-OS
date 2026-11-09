// AMPLIFY-OS Pearl Stage C — Identity login and lock surface.
#include "login.h"
#include "users.h"
#include "keyboard.h"
#include "serial.h"
#include "timer.h"
#include "rtc.h"
#include "graphics.h"
#include "doublebuf.h"
#include "ui_theme.h"
#include "ui_layout.h"
#include "ui_icons.h"
#include "power.h"
#include <stdint.h>
#include <stdbool.h>

static login_session_t g_session;static bool g_logged_in;
const login_session_t* login_current(void){return g_logged_in?&g_session:0;}
static int len(const char*s){int n=0;while(s&&s[n])n++;return n;}
static void copy(char*d,const char*s,int cap){int i=0;for(;s&&i<cap-1&&s[i];i++)d[i]=s[i];d[i]=0;}
static void text(const gfx_target_t*t,int x,int y,const char*s,uint32_t c){gfx_draw_string_transparent(t,ui_ux(x),ui_uy(y),s,c);}
static void bold(const gfx_target_t*t,int x,int y,const char*s,uint32_t c){text(t,x,y,s,c);gfx_draw_string_transparent(t,ui_ux(x)+1,ui_uy(y),s,c);}
static void scaled(const gfx_target_t*t,int x,int y,const char*s,uint32_t c,int z){gfx_draw_string_scaled(t,ui_ux(x),ui_uy(y),s,c,z);}
static void fill(const gfx_target_t*t,int x,int y,int w,int h,uint32_t c){gfx_fill_rect(t,ui_ux(x),ui_uy(y),ui_uw(w),ui_uh(h),c);}
static void round_fill(const gfx_target_t*t,int x,int y,int w,int h,int r,uint32_t c){gfx_fill_round_rect(t,ui_ux(x),ui_uy(y),ui_uw(w),ui_uh(h),ui_uw(r),c);}
static void time_buf(char out[6],const rtc_time_t*r){out[0]='0'+r->hour/10;out[1]='0'+r->hour%10;out[2]=':';out[3]='0'+r->minute/10;out[4]='0'+r->minute%10;out[5]=0;}
static void date_buf(char out[32],const rtc_time_t*r){
    const char*m="JAN";static const char*ms[12]={"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};if(r->month>=1&&r->month<=12)m=ms[r->month-1];
    int n=0;const char*p="IDENTITY - ";while(*p)out[n++]=*p++;out[n++]='0'+r->day/10;out[n++]='0'+r->day%10;out[n++]=' ';while(*m)out[n++]=*m++;out[n++]=' ';out[n++]='-';out[n++]=' ';p="YEKATERINBURG";while(*p)out[n++]=*p++;out[n]=0;
}
static void draw_field(const gfx_target_t*t,int x,int y,const char*pass,bool focus,int frame){
    const ui_runtime_theme_t*th=ui_theme_current();int n=len(pass);for(int i=0;i<4;i++){round_fill(t,x+i*66,y,54,58,8,th->border);round_fill(t,x+1+i*66,y+1,52,56,7,th->surface);if(i<n)scaled(t,x+18+i*66,y+18,"*",th->accent_coral,2);}
    if(focus&&((frame/10)&1)==0)fill(t,x+18+(n<4?n:3)*66,y+14,2,30,th->accent_berry);
}
static void draw_identity(const gfx_target_t*t,const char*name,const char*pass,bool focus_pass,const char*status,uint32_t status_color,int frame){
    const ui_runtime_theme_t*th=ui_theme_current();rtc_time_t rt;rtc_read(&rt);char tm[6],dt[32];time_buf(tm,&rt);date_buf(dt,&rt);
    gfx_clear(t,th->canvas);ui_draw_bitmap_alpha(t,ui_ux(-70),ui_uy(-35),ui_uw(1100),ui_uh(1100),ui_logo_big(),18);ui_draw_bitmap_scaled(t,ui_ux(80),ui_uy(78),ui_uw(48),ui_uh(48),ui_logo_small());
    scaled(t,80,750,tm,th->ink,5);text(t,80,858,dt,th->muted);round_fill(t,80,894,106,28,7,th->surface);text(t,94,901,"MESH SECURED",th->muted);round_fill(t,198,894,132,28,7,th->surface);text(t,212,901,"SPACES PARKED",th->muted);
    int x=1010,y=240,w=470,h=436;round_fill(t,x,y,w,h,18,th->border);round_fill(t,x+1,y+1,w-2,h-2,17,th->surface);
    for(int i=0;i<74;i++) fill(t,x+42+i,y+40,1,74,ui_sunset((uint8_t)(i*255/74)));
    scaled(t,x+68,y+65,"A",th->on_accent,2);
    bold(t,x+42,y+142,name[0]?name:"Guest strata",th->ink);text(t,x+42,y+172,users_count()?"Amplify ID - CHRT key present":"Ephemeral guest identity",th->muted);
    draw_field(t,x+42,y+224,pass,focus_pass,frame);text(t,x+42,y+294,"PASSPHRASE - 4 FACETS",th->muted);fill(t,x+42,y+330,w-84,1,th->border);
    round_fill(t,x+42,y+354,112,30,8,focus_pass?th->surface_soft:th->accent_coral);text(t,x+56,y+361,"USE KEY",focus_pass?th->ink:th->on_accent);text(t,x+w-144,y+361,"GUEST STRATA",th->muted);
    text(t,x+42,y+404,status,status_color);text(t,80,952,"Up/Down user  Tab field  Enter unlock  -  empty name: R restart, X shutdown",th->muted);
}
bool login_try_passphrase(const char*pass,login_session_t*out){
    if(!pass||!pass[0])return false;
    for(int i=0;i<users_count();i++){
        const user_record_t*u=users_at(i);
        if(!u||!users_verify(u->name,pass))continue;
        copy(g_session.name,u->name,sizeof(g_session.name));g_session.is_guest=false;
        g_session.login_ms=(uint32_t)timer_get_uptime_ms();g_logged_in=true;if(out)*out=g_session;
        serial_puts("[identity] unlocked user=");serial_puts(g_session.name);serial_puts(" (via disk unlock)\n");
        return true;
    }
    return false;
}
void login_run(login_session_t*out){
    char name[LOGIN_NAME_MAX]={0},pass[LOGIN_PASS_MAX]={0},status[64];int user_index=0,attempts=0,frame=0;bool focus_pass=false;uint32_t status_color=UI_MUTED;
    if(users_count()>0){const user_record_t*u=users_at(0);if(u)copy(name,u->name,sizeof(name));copy(status,"Enter your four-facet passphrase",sizeof(status));}else{copy(status,"Guest strata is ready",sizeof(status));status_color=UI_FG_OK;}
    for(;;frame++){
        const gfx_target_t*t=doublebuf_target();if(!t)return;ui_layout_update(t->width,t->height);draw_identity(t,name,pass,focus_pass,status,status_color,frame);doublebuf_swap();timer_sleep(40);
        if(!keyboard_has_char()) continue;
        char c=keyboard_wait_char();
        // [этап 57] R/X — команды питания только когда фокус на ПУСТОМ поле
        // имени.  Раньше они срабатывали всегда, и пароль (заданный, например,
        // в установщике) с буквой r или x было невозможно ввести: машина
        // уходила в перезагрузку посреди набора.
        if(!focus_pass&&!name[0]){if(c=='r'||c=='R') power_reboot_with_curtain();if(c=='x'||c=='X') power_shutdown_with_curtain();}
        if(c==KBD_KEY_UP||c==KBD_KEY_DOWN){int count=users_count();if(count){user_index=(user_index+(c==KBD_KEY_UP?count-1:1))%count;const user_record_t*u=users_at(user_index);if(u)copy(name,u->name,sizeof(name));pass[0]=0;}continue;}
        if(c=='\t'||c==KBD_KEY_BACKTAB){focus_pass=!focus_pass;continue;}
        if(c=='\b'){char*b=focus_pass?pass:name;int n=len(b);if(n)b[n-1]=0;continue;}
        if(c=='\n'||c=='\r'){
            if(!focus_pass){focus_pass=true;continue;}
            if(!users_verify(name,pass)){attempts++;copy(status,"Identity rejected - try again",sizeof(status));status_color=UI_FG_ERROR;pass[0]=0;if(attempts>=5){draw_identity(t,name,pass,true,"Cooling identity gate for 2 seconds",UI_FG_ERROR,frame);doublebuf_swap();timer_sleep(2000);attempts=0;}continue;}
            copy(g_session.name,name[0]?name:"guest",sizeof(g_session.name));g_session.is_guest=users_count()==0;g_session.login_ms=(uint32_t)timer_get_uptime_ms();g_logged_in=true;if(out)*out=g_session;
            serial_puts("[identity] unlocked user=");serial_puts(g_session.name);serial_puts("\n");draw_identity(t,g_session.name,pass,true,"Identity accepted - opening Apex",UI_FG_OK,frame);doublebuf_swap();timer_sleep(420);return;
        }
        if(c>=32&&c<127){char*b=focus_pass?pass:name;int cap=focus_pass?(int)sizeof(pass):(int)sizeof(name);int n=len(b);if(n<cap-1){b[n]=c;b[n+1]=0;}status_color=UI_MUTED;copy(status,"Enter to unlock",sizeof(status));}
    }
}

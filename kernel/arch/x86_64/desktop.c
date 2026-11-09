// AMPLIFY-OS Pearl Stage A — Apex workspace shell.
#include "desktop.h"
#include "graphics.h"
#include "rtc.h"
#include "ui_theme.h"
#include "ui_icons.h"
#include "ui_layout.h"
#include "ui_components.h"
#include "ui_overlay.h"
#include "beacon.h"
#include "login.h"
#include "timer.h"
#include <stdint.h>
#include <stdbool.h>

static desktop_icon_t g_icons[DESKTOP_MAX_ICONS];
static int g_n_icons;
static int g_pointer_x=-1, g_pointer_y=-1;
static int g_hover_slot=-1, g_focus_slot=0;

static const ui_icon_id_t g_rail_icons[9] = {
    UI_ICON_AMPLIFY, UI_ICON_FILES, UI_ICON_TERMINAL, UI_ICON_GALLERY,
    UI_ICON_NOTES, UI_ICON_MAIL, UI_ICON_CALENDAR, UI_ICON_SETTINGS, UI_ICON_STORE
};

static void copy_str(char* d,const char* s,int cap){int i=0;for(;i<cap-1&&s[i];i++)d[i]=s[i];d[i]=0;}
static bool eq(const char*a,const char*b){while(*a&&*b&&*a==*b){a++;b++;}return *a==0&&*b==0;}
static desktop_icon_t* find_icon(const char* name){for(int i=0;i<g_n_icons;i++)if(eq(g_icons[i].name,name))return &g_icons[i];return 0;}

void desktop_init(void){g_n_icons=0;g_pointer_x=g_pointer_y=-1;g_hover_slot=-1;g_focus_slot=0;ui_theme_init();}
int desktop_add_icon(const char* name,char glyph,uint32_t accent,desktop_click_fn cb){
    if(g_n_icons>=DESKTOP_MAX_ICONS)return -1;
    desktop_icon_t*ic=&g_icons[g_n_icons];copy_str(ic->name,name?name:"?",sizeof(ic->name));
    ic->glyph=glyph;ic->accent=accent;ic->icon_id=(int)ui_icon_by_name(name);ic->on_click=cb;
    ic->x=ic->y=ic->w=ic->h=0;return g_n_icons++;
}
void desktop_set_pointer(int x,int y){g_pointer_x=x;g_pointer_y=y;g_hover_slot=-1;for(int i=0;i<9;i++)if(ui_hit_logical(x,y,10,84+i*66,44,52))g_hover_slot=i;}

static void draw_text(const gfx_target_t*t,int x,int y,const char*s,uint32_t c){gfx_draw_string_transparent(t,ui_ux(x),ui_uy(y),s,c);}
static void draw_bold(const gfx_target_t*t,int x,int y,const char*s,uint32_t c){draw_text(t,x,y,s,c);gfx_draw_string_transparent(t,ui_ux(x)+1,ui_uy(y),s,c);}
static void draw_scaled(const gfx_target_t*t,int x,int y,const char*s,uint32_t c,int scale){gfx_draw_string_scaled(t,ui_ux(x),ui_uy(y),s,c,scale);}
static void fill(const gfx_target_t*t,int x,int y,int w,int h,uint32_t c){gfx_fill_rect(t,ui_ux(x),ui_uy(y),ui_uw(w),ui_uh(h),c);}
static void round_fill(const gfx_target_t*t,int x,int y,int w,int h,int r,uint32_t c){gfx_fill_round_rect(t,ui_ux(x),ui_uy(y),ui_uw(w),ui_uh(h),ui_uw(r),c);}

static void format_time(char out[6],const rtc_time_t*rt){out[0]='0'+rt->hour/10;out[1]='0'+rt->hour%10;out[2]=':';out[3]='0'+rt->minute/10;out[4]='0'+rt->minute%10;out[5]=0;}
static void format_date(char out[16],const rtc_time_t*rt){
    out[0]='0'+rt->day/10;out[1]='0'+rt->day%10;out[2]='.';out[3]='0'+rt->month/10;out[4]='0'+rt->month%10;out[5]='.';
    out[6]='0'+(rt->year/1000)%10;out[7]='0'+(rt->year/100)%10;out[8]='0'+(rt->year/10)%10;out[9]='0'+rt->year%10;out[10]=0;
}

static void draw_wallpaper(const gfx_target_t*t){
    const ui_runtime_theme_t*th=ui_theme_current();gfx_clear(t,th->canvas);
    fill(t,64,48,1536,952,th->canvas);
    const ui_bitmap_t*logo=ui_logo_big();
    ui_draw_bitmap_alpha(t,ui_ux(950),ui_uy(305),ui_uw(610),ui_uh(610),logo,22);
}

static void draw_top_status(const gfx_target_t*t,const rtc_time_t*rt){
    const ui_runtime_theme_t*th=ui_theme_current();char tm[6];format_time(tm,rt);
    round_fill(t,1236,10,344,28,10,th->surface);
    draw_text(t,1250,16,"My Pony",th->ink);draw_text(t,1340,16,"NET",th->muted);
    draw_text(t,1392,16,"VM",th->muted);draw_text(t,1438,16,tm,th->ink);
    fill(t,1218,18,5,5,0xFF46A171);
}

static void draw_focus_card(const gfx_target_t*t){
    const ui_runtime_theme_t*th=ui_theme_current();ui_draw_card(t,142,329,470,234,18);
    ui_draw_section_label(t,168,356,"RESUME STRATA");draw_text(t,528,356,"3 WARM",th->muted);
    round_fill(t,168,385,42,42,10,th->accent_coral);ui_draw_bitmap_scaled(t,ui_ux(177),ui_uy(394),ui_uw(24),ui_uh(24),ui_icon(UI_ICON_TERMINAL));
    draw_bold(t,225,389,"release - 2.9.0",th->ink);draw_text(t,225,411,"rollback armed - eu-west",th->muted);ui_draw_button(t,534,390,52,30,"live",false,UI_CONTROL_IDLE);
    round_fill(t,168,443,42,42,10,th->surface_soft);ui_draw_bitmap_scaled(t,ui_ux(177),ui_uy(452),ui_uw(24),ui_uh(24),ui_icon(UI_ICON_NOTES));
    draw_bold(t,225,447,"Thread - CHRT spec",th->ink);draw_text(t,225,469,"edited 12 min ago",th->muted);
    round_fill(t,168,501,42,42,10,th->surface_soft);ui_draw_bitmap_scaled(t,ui_ux(177),ui_uy(510),ui_uw(24),ui_uh(24),ui_icon(UI_ICON_FILES));
    draw_bold(t,225,505,"Strata - 4 drops",th->ink);draw_text(t,225,527,"synced across 3 devices",th->muted);
}

static void draw_agenda(const gfx_target_t*t){
    const ui_runtime_theme_t*th=ui_theme_current();
    ui_draw_button(t,142,588,66,28,"ring",false,UI_CONTROL_IDLE);ui_draw_button(t,220,588,70,28,"beacon",false,UI_CONTROL_IDLE);ui_draw_button(t,302,588,86,28,"spaces - 4",false,UI_CONTROL_IDLE);
    ui_draw_card(t,142,647,224,119,14);draw_text(t,165,670,"SIGNALS",th->muted);draw_scaled(t,165,700,"2 need you",th->ink,2);draw_text(t,165,738,"Everything else waits.",th->muted);
    ui_draw_card(t,388,647,224,119,14);draw_text(t,411,670,"NEXT ON THE ARC",th->muted);draw_scaled(t,411,700,"16:00",th->ink,2);draw_text(t,411,738,"Ring review - decision.",th->muted);
}

static void draw_clock_hub(const gfx_target_t*t,const rtc_time_t*rt){
    const ui_runtime_theme_t*th=ui_theme_current();char tm[6],date[16];format_time(tm,rt);format_date(date,rt);
    int cx=1102,cy=506;ui_draw_ring(t,cx,cy,194,4,174,th->border,th->accent_coral);ui_draw_ring(t,cx,cy,146,2,120,th->border,th->accent_berry);
    round_fill(t,cx-113,cy-113,226,226,113,th->surface);draw_scaled(t,cx-54,cy-48,tm,th->ink,3);draw_text(t,cx-48,cy+14,date,th->muted);ui_draw_button(t,cx-52,cy+42,104,30,"focus - deep",false,UI_CONTROL_IDLE);
    static const int px[8]={1102,1250,1338,1338,1250,1102,954,866};
    static const int py[8]={258,302,394,548,640,686,640,548};
    static const ui_icon_id_t ii[8]={UI_ICON_NOTES,UI_ICON_MAIL,UI_ICON_CALENDAR,UI_ICON_MUSIC,UI_ICON_SETTINGS,UI_ICON_TERMINAL,UI_ICON_FILES,UI_ICON_GALLERY};
    static const char* labels[8]={"NOTES","MAIL","CALENDAR","SOUND","CORE","CONSOLE","FILES","GALLERY"};
    for(int i=0;i<8;i++){round_fill(t,px[i]-37,py[i]-37,74,74,12,i==5?th->accent_coral:th->surface);ui_draw_bitmap_scaled(t,ui_ux(px[i]-12),ui_uy(py[i]-12),ui_uw(24),ui_uh(24),ui_icon(ii[i]));draw_text(t,px[i]-28,py[i]+48,labels[i],th->muted);}
}

static void draw_footer_cards(const gfx_target_t*t){
    const ui_runtime_theme_t*th=ui_theme_current();
    draw_text(t,142,816,"Super+N Beacon  -  Super+Tab Constellation  -  Super+Space Pulse",th->muted);
    ui_draw_card(t,142,850,470,70,14);draw_text(t,165,870,"SYSTEM PULSE",th->muted);draw_bold(t,300,870,"All systems nominal",th->ink);fill(t,165,902,420,3,th->border);fill(t,165,902,356,3,th->accent_coral);
}

void desktop_draw(const gfx_target_t*t){
    if(!t) return;
    ui_layout_update(t->width,t->height);
    const ui_runtime_theme_t*th=ui_theme_current();
    rtc_time_t rt;
    rtc_read(&rt);
    draw_wallpaper(t);
    int indicated = g_hover_slot >= 0 ? g_hover_slot : g_focus_slot;
    ui_draw_app_shell(t,0,indicated,g_rail_icons,9);
    draw_top_status(t,&rt);
    draw_text(t,142,124,"APEX - PERSONAL CORE",th->muted);draw_scaled(t,142,156,"Good afternoon,",th->ink,2);draw_scaled(t,142,200,"My Pony",th->accent_berry,2);
    draw_text(t,142,258,"Three strata are warm. Nothing is waiting on you - the ring keeps your",th->muted);
    draw_text(t,142,280,"next move one gesture away.",th->muted);
    draw_focus_card(t);draw_agenda(t);draw_clock_hub(t,&rt);draw_footer_cards(t);
    draw_text(t,112,934,"1-9 navigate  /  Enter opens  /  Core holds every prism",th->muted);
}

// ----------------------------------------------------------------------------
// [41.2] Ключ состояния рабочего стола: всё, от чего зависит картинка
// desktop_draw (подсвеченный слот rail, минута/дата, тема). WM держит готовый
// слой рабочего стола в кэше и перерисовывает его, только когда ключ сменился,
// а не на каждый кадр (отрисовка стола — 12 мс из 17 мс кадра под KVM).
// RTC читаем не чаще раза в секунду: это десяток обращений к портам CMOS,
// под KVM каждое — выход в гипервизор.
// ----------------------------------------------------------------------------
uint64_t desktop_state_key(void){
    static uint64_t last_ms=~0ULL; static uint64_t stamp=0;
    uint64_t now=timer_get_uptime_ms();
    if(last_ms==~0ULL||now-last_ms>=1000){
        rtc_time_t rt; rtc_read(&rt);
        stamp=(uint64_t)rt.minute|((uint64_t)rt.hour<<6)|((uint64_t)rt.day<<11)|((uint64_t)rt.month<<16)|((uint64_t)rt.year<<20);
        last_ms=now;
    }
    uint64_t h=1469598103934665603ULL;               // FNV-1a
    const uint8_t* th=(const uint8_t*)ui_theme_current();
    for(unsigned i=0;i<sizeof(ui_runtime_theme_t);i++){h^=th[i];h*=1099511628211ULL;}
    int indicated=g_hover_slot>=0?g_hover_slot:g_focus_slot;
    h^=(uint64_t)(uint32_t)indicated;h*=1099511628211ULL;
    h^=stamp;h*=1099511628211ULL;
    return h;
}

static void launch_slot(int slot){
    static const char* map[9]={0,"Files","Term","Facet","Notes","Signal","Arc","Core","Amply"};
    if(slot<=0||slot>=9) return;
    if (!map[slot]) return;
    desktop_icon_t*ic=find_icon(map[slot]);
    if(ic&&ic->on_click) ic->on_click();
}
bool desktop_handle_click(int mx,int my){
    for(int i=0;i<9;i++)if(ui_hit_logical(mx,my,10,84+i*66,44,52)){g_focus_slot=i;launch_slot(i);return true;}
    if(ui_hit_logical(mx,my,1236,8,344,34)){
        beacon_show();
        return true;
    }
    if(ui_hit_logical(mx,my,14,790,36,36)){
        login_session_t unlocked;
        login_run(&unlocked);
        return true;
    }
    if(ui_hit_logical(mx,my,14,848,36,36)){
        ui_theme_set_dark(!ui_theme_current()->dark);
        ui_overlay_show_toast(ui_theme_current()->dark?"Dark Pearl enabled":"Light Pearl enabled",1800);
        return true;
    }
    if(ui_hit_logical(mx,my,14,906,36,36)){desktop_launch_named("Core");return true;}
    if(ui_hit_logical(mx,my,438,366,78,32)){desktop_icon_t*ic=find_icon("About");if(ic&&ic->on_click)ic->on_click();return true;}
    return false;
}
bool desktop_handle_key(char c){
    if(c>='1'&&c<='9'){g_focus_slot=c-'1';return true;}
    if(c=='\t'){g_focus_slot=(g_focus_slot+1)%9;return true;}
    if(c==28){g_focus_slot=(g_focus_slot+8)%9;return true;}
    if(c=='\n'||c==' '){launch_slot(g_focus_slot);return true;}
    if(c==27){g_focus_slot=0;return true;}
    return false;
}

// ----------------------------------------------------------------------------
// Реестр приложений — единый источник правды для Core (этап F).
// ----------------------------------------------------------------------------
int desktop_icon_count(void){return g_n_icons;}
const desktop_icon_t* desktop_icon_at(int index){
    if(index<0||index>=g_n_icons) return 0;
    return &g_icons[index];
}
bool desktop_launch_named(const char* name){
    desktop_icon_t* ic=find_icon(name);
    if(ic&&ic->on_click){ic->on_click();return true;}
    return false;
}

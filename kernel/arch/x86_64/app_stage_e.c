#include "app_stage_e.h"
#include "wm.h"
#include "compositor.h"
#include "ui_icons.h"
#include "rtc.h"
int se_len(const char*s){int n=0;while(s&&s[n])n++;return n;}
void se_copy(char*d,const char*s,int cap){int i=0;for(;s&&i<cap-1&&s[i];i++)d[i]=s[i];d[i]=0;}
bool se_contains(const char*s,const char*q){if(!q||!q[0])return true;for(int i=0;s&&s[i];i++){int j=0;while(q[j]&&s[i+j]==q[j])j++;if(!q[j])return true;}return false;}
void se_text(const gfx_target_t*t,int x,int y,const char*s,uint32_t c){gfx_draw_string_transparent(t,x,y,s,c);}
void se_bold(const gfx_target_t*t,int x,int y,const char*s,uint32_t c){se_text(t,x,y,s,c);gfx_draw_string_transparent(t,x+1,y,s,c);}
void se_card(const gfx_target_t*t,int x,int y,int w,int h,uint32_t s,uint32_t b){gfx_fill_round_rect(t,x,y,w,h,14,b);gfx_fill_round_rect(t,x+1,y+1,w-2,h-2,13,s);}
void se_num(char*out,uint64_t v){char a[24];int i=0,n=0;if(!v)a[i++]='0';while(v){a[i++]=(char)('0'+v%10);v/=10;}while(i)out[n++]=a[--i];out[n]=0;}
void se_close(window_t*w){if(compositor_active()&&!compositor_is_animating(w))compositor_anim_close(w);else{w->visible=false;w->close_requested=true;}}
window_t* se_workspace(const char*title,window_draw_fn d,window_click_fn c,window_key_fn k,void*s,bool dark){const gfx_target_t*g=gfx_screen();int sw=g?g->width:1600,sh=g?g->height:1000,left=sw*64/1600,top=sh*48/1000;window_t*w=window_create(title,left,top,sw-left,sh-top);if(!w)return 0;w->on_draw=d;w->on_click=c;w->on_key=k;w->user_data=s;w->owns_user_data=(s!=0);w->bg_color=dark?UI_DARK_CANVAS:UI_CANVAS;w->accent_color=UI_ACCENT_CORAL;window_set_chrome(w,WINDOW_CHROME_WORKSPACE);wm_add_window(w);return w;}
void se_header(const gfx_target_t*t,const char*kicker,const char*title,bool dark){uint32_t muted=dark?0xFFA99FAA:ui_theme_current()->muted,ink=dark?UI_DARK_TEXT:ui_theme_current()->ink;se_text(t,78,28,kicker,muted);se_bold(t,78,55,title,ink);rtc_time_t r;rtc_read(&r);char tm[6]={(char)('0'+r.hour/10),(char)('0'+r.hour%10),':',(char)('0'+r.minute/10),(char)('0'+r.minute%10),0};se_card(t,t->width-316,14,276,42,dark?0xFF29222D:ui_theme_current()->surface,dark?0xFF403744:ui_theme_current()->border);se_text(t,t->width-292,28,"Mesh     86%",muted);se_bold(t,t->width-132,28,tm,ink);}
void se_progress(const gfx_target_t*t,int x,int y,int w,uint8_t p,uint32_t track,uint32_t accent){gfx_fill_rect(t,x,y,w,6,track);gfx_fill_rect_gradient(t,x,y,w*p/255,6,UI_ACCENT_ORANGE,accent);}
void se_ip(char out[16],uint32_t ip){int p=0;for(int n=3;n>=0;n--){uint8_t v=(uint8_t)(ip>>(n*8));char a[3];int k=0;if(v>=100)a[k++]=(char)('0'+v/100);if(v>=10)a[k++]=(char)('0'+(v/10)%10);a[k++]=(char)('0'+v%10);for(int i=0;i<k;i++)out[p++]=a[i];if(n)out[p++]='.';}out[p]=0;}

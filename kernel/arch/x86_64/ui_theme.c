#include "ui_theme.h"
#include "fat32.h"

static ui_runtime_theme_t g_theme;
static bool g_initialized = false;

static void set_light(void) {
    bool reduced = g_initialized ? g_theme.reduced_motion : false;
    g_theme.canvas        = 0xFFF7EEF4;
    g_theme.surface       = 0xFFFFFDFE;
    g_theme.surface_soft  = 0xFFF1E5ED;
    g_theme.ink           = 0xFF241F27;
    g_theme.muted         = 0xFF756C77;
    g_theme.border        = 0xFFE7D9E2;
    g_theme.accent_orange = 0xFFF0A33A;
    g_theme.accent_coral  = 0xFFE8785D;
    g_theme.accent_berry  = 0xFFB54478;
    g_theme.on_accent     = 0xFFFFFFFF;
    g_theme.shadow        = 0x24582046;
    g_theme.dark = false;
    g_theme.reduced_motion = reduced;
}
static void set_dark(void) {
    bool reduced = g_initialized ? g_theme.reduced_motion : false;
    g_theme.canvas        = 0xFF151019;
    g_theme.surface       = 0xFF211923;
    g_theme.surface_soft  = 0xFF2A202C;
    g_theme.ink           = 0xFFF8F3F7;
    g_theme.muted         = 0xFFA99FAA;
    g_theme.border        = 0xFF352A36;
    g_theme.accent_orange = 0xFFF0A33A;
    g_theme.accent_coral  = 0xFFE8785D;
    g_theme.accent_berry  = 0xFFB54478;
    g_theme.on_accent     = 0xFFFFFFFF;
    g_theme.shadow        = 0x66000000;
    g_theme.dark = true;
    g_theme.reduced_motion = reduced;
}

void ui_theme_init(void) {
    set_light(); g_initialized = true;
    /* Stage E: desktop_init runs after FAT32 mount, so the active Facet can
       be restored without introducing an early-boot filesystem dependency. */
    char cfg[64];
    int64_t n=fat32_read_file("/disk/Sanctum/system/ui-config.ini",cfg,sizeof(cfg)-1);
    if(n>0){
        cfg[n]=0; int preset=0; bool dark=false;
        for(int i=0;i+6<n;i++){
            if(cfg[i]=='t'&&cfg[i+1]=='h'&&cfg[i+6]>='0'&&cfg[i+6]<='5')preset=cfg[i+6]-'0';
            if(cfg[i]=='d'&&cfg[i+1]=='a'&&cfg[i+5]=='1')dark=true;
        }
        if(dark)set_dark();
        static const uint32_t a[6]={0xFFF0A33A,0xFFC9B6DC,0xFFE9A03B,0xFF9EBBE0,0xFF91C5B0,0xFFF2C889};
        static const uint32_t b[6]={0xFFCC5A82,0xFFB796C8,0xFFD56D8A,0xFF7A9DCE,0xFF69AA91,0xFFDB9E72};
        g_theme.accent_orange=a[preset]; g_theme.accent_coral=ui_lerp(a[preset],b[preset],128); g_theme.accent_berry=b[preset];
    }
}
const ui_runtime_theme_t* ui_theme_current(void) {
    if (!g_initialized) ui_theme_init();
    return &g_theme;
}
void ui_theme_set_dark(bool dark) { if (dark) set_dark(); else set_light(); g_initialized=true; }
void ui_theme_set_accent(uint32_t orange, uint32_t coral, uint32_t berry) {
    if (!g_initialized) ui_theme_init();
    g_theme.accent_orange=orange; g_theme.accent_coral=coral; g_theme.accent_berry=berry;
}
void ui_theme_set_reduced_motion(bool enabled) {
    if (!g_initialized) ui_theme_init();
    g_theme.reduced_motion = enabled;
}
bool ui_theme_reduced_motion(void) {
    return ui_theme_current()->reduced_motion;
}

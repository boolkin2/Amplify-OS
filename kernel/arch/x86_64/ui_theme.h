// ============================================================================
// ui_theme.h — единая дизайн-система AMPLIFY-OS x86_64: тема «Pearl».
// ----------------------------------------------------------------------------
// Полный редизайн: светлый перламутрово-розовый интерфейс, закатный
// градиент Amplify (orange → magenta) как единственный акцент,
// растровые иконки из вектора (ui_icons.h), стеклянные панели.
//
// Имена макросов сохранены — остальной код (окна, splash, login, приложения)
// автоматически получает новую тему.
// ============================================================================
#ifndef AOS_X86_64_UI_THEME_H
#define AOS_X86_64_UI_THEME_H

#include <stdint.h>
#include <stdbool.h>

// Runtime-токены Stage A. Старые макросы ниже временно сохранены для
// совместимости с приложениями, которые будут мигрировать поэтапно.
typedef struct {
    uint32_t canvas;
    uint32_t surface;
    uint32_t surface_soft;
    uint32_t ink;
    uint32_t muted;
    uint32_t border;
    uint32_t accent_orange;
    uint32_t accent_coral;
    uint32_t accent_berry;
    uint32_t on_accent;
    uint32_t shadow;
    bool dark;
    bool reduced_motion;
} ui_runtime_theme_t;

void ui_theme_init(void);
const ui_runtime_theme_t* ui_theme_current(void);
void ui_theme_set_dark(bool dark);
void ui_theme_set_accent(uint32_t orange, uint32_t coral, uint32_t berry);
void ui_theme_set_reduced_motion(bool enabled);
bool ui_theme_reduced_motion(void);

#define UI_CANVAS           0xFFF7EEF4
#define UI_SURFACE          0xFFFFFDFE
#define UI_SURFACE_SOFT     0xFFF1E5ED
#define UI_INK              0xFF241F27
#define UI_MUTED            0xFF756C77
#define UI_BORDER           0xFFE7D9E2
#define UI_PEARL_ORANGE     0xFFF0A33A
#define UI_PEARL_CORAL      0xFFE8785D
#define UI_PEARL_BERRY      0xFFB54478
#define UI_DARK_CANVAS      0xFF151019
#define UI_DARK_SURFACE     0xFF211923
#define UI_DARK_BORDER      0xFF352A36
#define UI_DARK_TEXT        0xFFF8F3F7

// ---------------------------------------------------------------- Палитра
#define UI_BG_DEEP          0xFFE3D5E2
#define UI_BG_DARK          0xFFF8F2F6
#define UI_BG_MID           0xFFF0E5EE
#define UI_BG_LIGHT         0xFFFFFFFF

#define UI_WALL_TOP         0xFFF9EEF4
#define UI_WALL_MID         0xFFF1E3EC
#define UI_WALL_BOT         0xFFE6D8E5

#define UI_ACCENT_ORANGE    0xFFEFB13F
#define UI_ACCENT_MAGENTA   0xFFBC4A7E
#define UI_ACCENT_CORAL     0xFFDE7E5C
#define UI_ACCENT_GOLD      0xFFE8A33D
#define UI_ACCENT_CYAN      0xFF4FB9C9

#define UI_FG_PRIMARY       0xFF2C2C2B
#define UI_FG_SECONDARY     0xFF6B6470
#define UI_FG_DIM           0xFF938C97
#define UI_FG_ERROR         0xFFD1483C
#define UI_FG_OK            0xFF2F8A5B
#define UI_FG_ON_ACCENT     0xFFFFFFFF

#define UI_BORDER_SOFT      0xFFEBE0E8
#define UI_BORDER_STRONG    0xFFD5C5D3
#define UI_SHADOW_ARGB      0x33582046
#define UI_GLASS_LIGHT      0xB8FFFFFF

// --------------------------------------------------------------- Геометрия
#define UI_PAD_S            6
#define UI_PAD_M            10
#define UI_PAD_L            16

#define UI_TITLE_H          28

#define UI_RADIUS_S         6
#define UI_RADIUS_M         10
#define UI_RADIUS_L         14

// ----------------------------------------------------------------- Helpers
static inline uint32_t ui_argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t ui_lerp(uint32_t a, uint32_t b, uint8_t t) {
    uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF, aa = (a >> 24) & 0xFF;
    uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF, ba = (b >> 24) & 0xFF;
    uint32_t r  = (ar * (255 - t) + br * t) / 255;
    uint32_t g  = (ag * (255 - t) + bg * t) / 255;
    uint32_t bl = (ab * (255 - t) + bb * t) / 255;
    uint32_t al = (aa * (255 - t) + ba * t) / 255;
    return (al << 24) | (r << 16) | (g << 8) | bl;
}

static inline uint32_t ui_lighten(uint32_t c, uint8_t amount) {
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF, a = (c >> 24) & 0xFF;
    r += ((255 - r) * amount) / 255;
    g += ((255 - g) * amount) / 255;
    b += ((255 - b) * amount) / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline uint32_t ui_darken(uint32_t c, uint8_t amount) {
    uint32_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF, a = (c >> 24) & 0xFF;
    r = (r * (255 - amount)) / 255;
    g = (g * (255 - amount)) / 255;
    b = (b * (255 - amount)) / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

// Точка закатного градиента Amplify: t = 0..255 (orange → coral → magenta).
static inline uint32_t ui_sunset(uint8_t t) {
    if (t < 128) return ui_lerp(UI_ACCENT_ORANGE, UI_ACCENT_CORAL, (uint8_t)(t * 2));
    return ui_lerp(UI_ACCENT_CORAL, UI_ACCENT_MAGENTA, (uint8_t)((t - 128) * 2));
}

#endif

// ============================================================================
// sec_ui.h - Внутренние UI-помощники для firstboot / login screen (Этап 32)
// ============================================================================
// Маленький собственный UI-toolkit, потому что полноценный wm_* нам тут не
// подходит: эти экраны рисуются ДО старта основного цикла оконного менеджера.
// ============================================================================

#ifndef SEC_UI_H
#define SEC_UI_H

#include <stdint.h>
#include <stdbool.h>

// Цвета темы (стиль ОС: глубокий тёмно-синий + бирюзовый акцент).
#define SECUI_BG_TOP        0xFF101830u
#define SECUI_BG_BOTTOM     0xFF20284Cu
#define SECUI_PANEL         0xFF1A2240u
#define SECUI_PANEL_BORDER  0xFF40A0FFu
#define SECUI_INPUT_BG      0xFF0E1430u
#define SECUI_INPUT_BORDER  0xFF40A0C0u
#define SECUI_INPUT_FG      0xFFE0F0FFu
#define SECUI_BTN_TOP       0xFF40A0FFu
#define SECUI_BTN_BOTTOM    0xFF1860C0u
#define SECUI_BTN_FG        0xFFFFFFFFu
#define SECUI_BTN_DISABLED  0xFF606878u
#define SECUI_TITLE_FG      0xFFFFFFFFu
#define SECUI_TEXT_FG       0xFFC8D8F0u
#define SECUI_TEXT_DIM      0xFF8090B0u
#define SECUI_ERROR_FG      0xFFFF6080u
#define SECUI_ACCENT        0xFF60E0E0u

// Тип поля ввода.
typedef enum {
    SECUI_FIELD_TEXT,       // обычный текст
    SECUI_FIELD_PASSWORD    // отображается звёздочками
} secui_field_kind_t;

// Состояние одного поля ввода.
typedef struct {
    int32_t  x, y, w, h;
    char*    buffer;            // указатель на буфер (живёт у вызывающего)
    int      buffer_size;       // размер буфера, включая \0
    int      cursor;            // позиция курсора в buffer
    secui_field_kind_t kind;
    bool     focused;
    const char* placeholder;
} secui_field_t;

// Обработка кадра поля: рисует поле и принимает символ из клавиатуры.
// Возвращает true, если был нажат Enter.
bool secui_field_handle(secui_field_t* f, int ch);

// Просто рисует поле без чтения клавиатуры.
void secui_field_draw(const secui_field_t* f);

// Кнопка.
typedef struct {
    int32_t  x, y, w, h;
    const char* label;
    bool     enabled;
} secui_button_t;

void secui_button_draw(const secui_button_t* b, bool hovered);

// Полностью отрисовать фон экрана (градиент + точечный паттерн).
void secui_draw_background(int frame_phase);

// Нарисовать текстовую панель с заголовком и подзаголовком (центр экрана).
// Возвращает y верхнего края панели и её высоту через out-параметры.
void secui_draw_panel(int32_t x, int32_t y, int32_t w, int32_t h,
                      const char* title, const char* subtitle);

// Презентовать кадр на экран (doublebuf_swap).
void secui_present(void);

// Получить следующий ASCII-символ из клавиатуры (неблокирующее), либо 0.
int secui_kb_poll(void);

// Маленький "spinner" / progress dots.
void secui_draw_spinner(int32_t cx, int32_t cy, int frame_phase);

// Печать вспомогательной строки (centered).
void secui_text_centered(int32_t x, int32_t y, int32_t w, const char* s,
                         uint32_t fg);

// Печать заголовка крупно (двойной размер 8x16 → 16x32 простым 2× scale).
void secui_text_big(int32_t x, int32_t y, const char* s, uint32_t fg, int scale);

// Длина строки.
int secui_strlen(const char* s);

// Сравнение строк.
int secui_strcmp(const char* a, const char* b);

// Копирование (не более max-1 символов).
void secui_strncpy(char* dst, const char* src, int max);

#endif // SEC_UI_H

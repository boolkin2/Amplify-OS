#ifndef AOS_X86_64_UI_MOTION_H
#define AOS_X86_64_UI_MOTION_H

#include <stdbool.h>
#include <stdint.h>

typedef enum { UI_EASE_LINEAR=0, UI_EASE_OUT_CUBIC, UI_EASE_IN_OUT } ui_ease_t;
typedef struct {
    uint64_t started_ms;
    uint32_t duration_ms;
    uint32_t delay_ms;
    ui_ease_t ease;
    bool running;
} ui_transition_t;

void ui_transition_start(ui_transition_t* tr, uint32_t duration_ms,
                         uint32_t delay_ms, ui_ease_t ease);
uint8_t ui_transition_progress(ui_transition_t* tr);
bool ui_transition_active(ui_transition_t* tr);
int ui_transition_lerp(int from, int to, uint8_t progress);

// [48.2] Замедление всех анимаций в N раз (1 — обычная скорость). Нужно,
// чтобы разглядеть или снять на скриншот середину движения, которое в жизни
// длится 0.3 с: `compstat slow 8` в терминале или `animslow=8` в perf.conf.
void     ui_motion_set_slow(uint32_t factor);
uint32_t ui_motion_slow(void);

#endif

#include "ui_motion.h"
#include "ui_theme.h"
#include "timer.h"

static uint8_t ease_value(ui_ease_t ease, uint8_t t) {
    if (ease == UI_EASE_LINEAR) return t;
    if (ease == UI_EASE_OUT_CUBIC) {
        uint32_t inv=255-t;
        uint32_t inv3=(inv*inv*inv)/(255u*255u);
        return (uint8_t)(255-inv3);
    }
    if (t < 128) {
        uint32_t x=t;
        return (uint8_t)((2u*x*x)/255u);
    }
    uint32_t x=255-t;
    return (uint8_t)(255u-(2u*x*x)/255u);
}

static uint32_t g_slow = 1;
void     ui_motion_set_slow(uint32_t factor) { g_slow = factor < 1 ? 1 : factor > 50 ? 50 : factor; }
uint32_t ui_motion_slow(void) { return g_slow; }

void ui_transition_start(ui_transition_t* tr, uint32_t duration_ms,
                         uint32_t delay_ms, ui_ease_t ease) {
    if (!tr) return;
    tr->started_ms=timer_get_uptime_ms();
    tr->duration_ms=(duration_ms?duration_ms:1)*g_slow;
    tr->delay_ms=delay_ms;
    tr->ease=ease;
    tr->running=!ui_theme_reduced_motion();
}

uint8_t ui_transition_progress(ui_transition_t* tr) {
    if (!tr || !tr->running) return 255;
    uint64_t now=timer_get_uptime_ms();
    uint64_t elapsed=now-tr->started_ms;
    if (elapsed <= tr->delay_ms) return 0;
    elapsed-=tr->delay_ms;
    if (elapsed >= tr->duration_ms) { tr->running=false; return 255; }
    uint8_t raw=(uint8_t)((elapsed*255u)/tr->duration_ms);
    return ease_value(tr->ease,raw);
}

bool ui_transition_active(ui_transition_t* tr) {
    if (!tr || !tr->running) return false;
    (void)ui_transition_progress(tr);
    return tr->running;
}

int ui_transition_lerp(int from, int to, uint8_t p) {
    return from + (int)(((int64_t)(to-from)*p)/255);
}

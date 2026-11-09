// ============================================================================
// speaker.c — реализация драйвера PC Speaker для x86_64.
// ============================================================================
#include "speaker.h"
#include "timer.h"
#include "io.h"

#define PIT_BASE_FREQ      1193182u
#define PIT_COMMAND        0x43
#define PIT_CHANNEL2       0x42
#define SPEAKER_PORT       0x61

void speaker_on(uint32_t frequency) {
    if (frequency == 0) return;

    uint32_t divisor = PIT_BASE_FREQ / frequency;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    if (divisor < 1)      divisor = 1;

    // PIT channel 2, mode 3 (square wave), binary, lobyte+hibyte.
    outb(PIT_COMMAND, 0xB6);
    outb(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)((divisor >> 8) & 0xFF));

    uint8_t tmp = inb(SPEAKER_PORT);
    if ((tmp & 3) != 3) {
        outb(SPEAKER_PORT, tmp | 3);
    }
}

void speaker_off(void) {
    uint8_t tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp & 0xFC);
}

void speaker_beep(uint32_t frequency, uint32_t duration_ms) {
    speaker_on(frequency);
    timer_sleep(duration_ms);
    speaker_off();
}

void speaker_play_boot_melody(void) {
    speaker_beep(NOTE_C5, 80);
    timer_sleep(20);
    speaker_beep(NOTE_E5, 80);
    timer_sleep(20);
    speaker_beep(NOTE_G5, 80);
    timer_sleep(20);
    speaker_beep(NOTE_C6, 150);
}

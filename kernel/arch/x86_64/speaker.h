// ============================================================================
// speaker.h — драйвер PC Speaker для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Использует PIT Channel 2 для генерации меандра. Те же порты:
//   0x42 — данные PIT канала 2
//   0x43 — командный регистр PIT
//   0x61 — управление динамиком (биты 0-1)
// ============================================================================
#ifndef AOS_X86_64_SPEAKER_H
#define AOS_X86_64_SPEAKER_H

#include <stdint.h>

// Частоты нот (Hz) — 4-я и 5-я октавы.
#define NOTE_C4    262
#define NOTE_D4    294
#define NOTE_E4    330
#define NOTE_F4    349
#define NOTE_G4    392
#define NOTE_A4    440
#define NOTE_B4    494
#define NOTE_C5    523
#define NOTE_D5    587
#define NOTE_E5    659
#define NOTE_F5    698
#define NOTE_G5    784
#define NOTE_A5    880
#define NOTE_B5    988
#define NOTE_C6    1047

// Включить динамик на заданной частоте (Hz). 0 → no-op.
void speaker_on(uint32_t frequency);

// Выключить динамик.
void speaker_off(void);

// Проиграть тон длительностью duration_ms.
void speaker_beep(uint32_t frequency, uint32_t duration_ms);

// Стартовая мелодия (мажорный аккорд C-E-G-C6).
void speaker_play_boot_melody(void);

#endif

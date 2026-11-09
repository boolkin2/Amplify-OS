// ============================================================================
// audio.h — высокоуровневый API звука для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Адаптер kernel/sound/audio.c под VFS-API нашего x86_64-ядра.
// Парсит WAV из VFS, делает nearest-neighbor ресемплинг до 48 kHz stereo и
// отдаёт в ac97_play(). Минимум зависимостей.
// ============================================================================
#ifndef AOS_X86_64_AUDIO_H
#define AOS_X86_64_AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Инициализация. Возвращает true если AC97 ready.
bool   audio_init(void);
bool   audio_is_ready(void);

void   audio_set_volume(uint8_t percent);
void   audio_stop(void);

// Сырые 16-bit signed stereo (interleaved L,R,L,R,...). sample_count = total
// (для stereo = 2 × число фреймов). Возвращает реально отправленных.
size_t audio_play_pcm(const int16_t* samples, size_t sample_count, uint32_t sample_rate);

// Воспроизвести WAV из VFS. PCM/16-bit/mono или stereo, любой rate.
bool   audio_play_wav(const char* path);

#endif

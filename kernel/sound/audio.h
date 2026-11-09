// ============================================================================
// audio.h — высокоуровневый API звука для MyOS (Этап 24)
// ----------------------------------------------------------------------------
// Абстракция над ac97: парсит WAV из VFS, конвертирует формат (если нужно),
// посылает PCM в ac97_play. Приложениям дано два простых вызова:
//
//   audio_init()            — вызывается из kernel.c после PCI-готовности.
//   audio_play_wav(path)    — открывает файл через VFS, играет его.
//
// Низкоуровневый audio_play_pcm позволяет проигрывать сырые 16-bit stereo
// samples (если, например, приложение генерирует синтез).
// ============================================================================

#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Инициализация подсистемы звука. Возвращает true при успехе.
// При неудаче (нет AC'97) все последующие audio_play_* — no-op.
bool audio_init(void);

// Готов ли звук.
bool audio_is_ready(void);

// Установить громкость (0..100 %).
void audio_set_volume(uint8_t percent);

// Проиграть сырые 16-bit signed stereo samples (чередование L,R,L,R,...).
// sample_count = число отсчётов (для stereo = 2 × число stereo-пар).
// Блокирующий вызов: возвращается после копирования в DMA-буферы
// (но не дожидаясь конца воспроизведения).
size_t audio_play_pcm(const int16_t* samples, size_t sample_count, uint32_t sample_rate);

// Проиграть WAV-файл из VFS.
// Поддерживаемые форматы: PCM uncompressed, 16-bit, mono или stereo,
// любая sample_rate (ресемплинг «nearest neighbor» до 48000 Hz).
// Возвращает true при успехе.
bool audio_play_wav(const char* path);

// Остановить воспроизведение.
void audio_stop(void);

#endif // AUDIO_H

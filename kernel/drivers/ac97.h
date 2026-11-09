// ============================================================================
// ac97.h — драйвер AC'97 аудиоконтроллера (Intel 82801AA) для MyOS
// ----------------------------------------------------------------------------
// AC'97 состоит из двух блоков регистров, доступных через IO-BAR'ы:
//
//   BAR0 → NAM  (Native Audio Mixer, 256 байт)   — codec: громкость, mute.
//   BAR1 → NABM (Native Audio Bus Master, 64 байта) — контроль DMA.
//
// DMA-передача описывается BDL (Buffer Descriptor List) — массив из 32
// записей. Каждая запись: физ. адрес буфера, кол-во samples, флаги
// (IOC — Interrupt On Completion, BUP — Buffer Underrun Policy).
//
// Поток PCM OUT использует регистры BAR1+0x10..0x1F (Playback).
// IRQ генерируется по завершению буфера с флагом IOC.
//
// Поддерживаемый формат: 16-bit stereo, 48000 Hz (нативный для AC'97).
// WAV-парсер в audio.c сам проверит/отконвертирует входные данные.
// ============================================================================

#ifndef AC97_H
#define AC97_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// --- NABM: Playback box (offsets от BAR1) ---
#define AC97_PO_BDBAR   0x10   // DWORD: физ. адрес BDL (выровнен на 8 байт)
#define AC97_PO_CIV     0x14   // BYTE:  Current Index Value (0..31, read-only)
#define AC97_PO_LVI     0x15   // BYTE:  Last Valid Index   — до какого индекса играть
#define AC97_PO_SR      0x16   // WORD:  Status Register    (IOC/FIFOE/BCIS/LVBCI/DCH)
#define AC97_PO_PICB    0x18   // WORD:  Position In Current Buffer (samples remaining)
#define AC97_PO_PIV     0x1A   // BYTE:  Prefetched Index Value
#define AC97_PO_CR      0x1B   // BYTE:  Control Register (RPBM, RR, LVBIE, FEIE, IOCE)

// NABM Global (BAR1 + 0x2C/2D/2E/30)
#define AC97_GLOB_CNT   0x2C   // DWORD: Global Control (GIE, Cold/Warm reset, etc.)
#define AC97_GLOB_STA   0x30   // DWORD: Global Status

// --- NAM (codec mixer) register offsets от BAR0 ---
#define AC97_NAM_RESET          0x00  // любая запись — codec reset
#define AC97_NAM_MASTER_VOLUME  0x02  // 0x0000 = громко (0 дБ), 0x8000 = mute
#define AC97_NAM_AUX_OUT_VOL    0x04
#define AC97_NAM_MONO_VOL       0x06
#define AC97_NAM_PCM_OUT_VOL    0x18  // громкость PCM
#define AC97_NAM_POWERDOWN      0x26

// --- PO_SR биты ---
#define AC97_SR_DCH         (1 << 0)
#define AC97_SR_CELV        (1 << 1)
#define AC97_SR_LVBCI       (1 << 2)
#define AC97_SR_BCIS        (1 << 3)
#define AC97_SR_FIFOE       (1 << 4)
// Write-1-to-clear: BCIS, LVBCI, FIFOE.

// --- PO_CR биты ---
#define AC97_CR_RPBM        (1 << 0)  // Run/Pause Bus Master
#define AC97_CR_RR          (1 << 1)  // Reset Registers
#define AC97_CR_LVBIE       (1 << 2)
#define AC97_CR_FEIE        (1 << 3)
#define AC97_CR_IOCE        (1 << 4)  // Interrupt On Completion Enable

// Размер BDL и параметры буферов. Один "буфер" = 2 × (samples × 2 байта).
#define AC97_BDL_ENTRIES        32
#define AC97_SAMPLES_PER_ENTRY  0x1000  // 4096 samples (L+R) → ~43 ms @ 48 kHz
// Значение должно быть < 0xFFFE и обычно = чётное число. 4K — классика.

// Буфер-дескриптор (8 байт, выравнен на 8).
typedef struct __attribute__((packed)) {
    uint32_t buf_ptr;        // физический адрес samples (16-bit signed stereo)
    uint16_t num_samples;    // количество samples в буфере (не байт!)
    uint16_t flags;          // бит 15 = IOC, бит 14 = BUP
} ac97_bdl_entry_t;

// Инициализация драйвера. Возвращает true при успехе.
// Если AC'97 не найден — возвращает false, ОС работает без звука.
bool ac97_init(void);

// Готов ли драйвер к воспроизведению.
bool ac97_is_ready(void);

// Воспроизвести PCM-блок. samples — указатель на 16-bit signed stereo данные
// (чередование L/R), sample_count — кол-во отсчётов (= кол-во stereo-пар
// × 2). Блок копируется во внутренний кольцевой буфер. Воспроизведение
// запускается автоматически.
//
// Возвращает кол-во принятых samples (может быть меньше, если буфер
// переполнен). sample_rate пока используется только для логов — AC'97
// играет на своей фиксированной частоте (обычно 48 kHz).
size_t ac97_play(const int16_t* samples, size_t sample_count, uint32_t sample_rate);

// Остановить воспроизведение и очистить очередь.
void ac97_stop(void);

// Установить master volume (0 = mute, 100 = max).
void ac97_set_volume(uint8_t percent);

#endif // AC97_H

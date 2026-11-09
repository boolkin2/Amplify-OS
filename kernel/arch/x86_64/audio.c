// ============================================================================
// audio.c — реализация audio API для x86_64.
// ----------------------------------------------------------------------------
// Логика портирована из kernel/sound/audio.c, но использует VFS-API нашего
// x86_64-ядра (vfs_open/vfs_size_fd/vfs_read/vfs_close) и serial для логов.
// ============================================================================

#include "audio.h"
#include "ac97.h"
#include "vfs.h"
#include "heap.h"
#include "serial.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define AC97_HW_RATE 48000u

// ----------------------------------------------------------------------------
// Структуры WAV.
// ----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    char     riff[4];
    uint32_t file_size;
    char     wave[4];
} wav_riff_header_t;

typedef struct __attribute__((packed)) {
    char     id[4];
    uint32_t size;
} wav_chunk_header_t;

typedef struct __attribute__((packed)) {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_fmt_t;

static inline bool tag_eq(const char* a, const char* b) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------
bool audio_init(void) { return ac97_init(); }
bool audio_is_ready(void) { return ac97_is_ready(); }
void audio_set_volume(uint8_t percent) { ac97_set_volume(percent); }
void audio_stop(void) { ac97_stop(); }

size_t audio_play_pcm(const int16_t* samples, size_t sample_count, uint32_t sample_rate) {
    if (!ac97_is_ready()) return 0;
    return ac97_play(samples, sample_count, sample_rate);
}

// ----------------------------------------------------------------------------
// audio_play_wav — открывает файл через VFS, парсит, ресемплит, играет.
// ----------------------------------------------------------------------------
bool audio_play_wav(const char* path) {
    serial_puts("[audio] play_wav: ");
    serial_puts(path ? path : "(null)");
    serial_puts("\n");

    if (!ac97_is_ready()) {
        serial_puts("[audio] no hardware\n");
        return false;
    }
    if (!path) return false;

    int fd = vfs_open(path);
    if (fd < 0) {
        serial_puts("[audio] open failed\n");
        return false;
    }

    int64_t filesize = vfs_size_fd(fd);
    if (filesize <= (int64_t)sizeof(wav_riff_header_t)) {
        serial_puts("[audio] file too small\n");
        vfs_close(fd);
        return false;
    }

    uint8_t* buf = (uint8_t*)kmalloc((size_t)filesize);
    if (!buf) {
        serial_puts("[audio] kmalloc failed\n");
        vfs_close(fd);
        return false;
    }

    int64_t nread = vfs_read(fd, buf, (uint64_t)filesize);
    vfs_close(fd);
    if (nread != filesize) {
        serial_puts("[audio] short read\n");
        kfree(buf);
        return false;
    }

    // Проверяем RIFF/WAVE.
    wav_riff_header_t* hdr = (wav_riff_header_t*)buf;
    if (!tag_eq(hdr->riff, "RIFF") || !tag_eq(hdr->wave, "WAVE")) {
        serial_puts("[audio] not a WAV\n");
        kfree(buf);
        return false;
    }

    // Ищем chunk'и fmt и data.
    uint8_t*   cursor    = buf + sizeof(wav_riff_header_t);
    uint8_t*   end       = buf + filesize;
    wav_fmt_t  fmt       = {0};
    uint8_t*   data_ptr  = NULL;
    uint32_t   data_size = 0;

    while (cursor + sizeof(wav_chunk_header_t) <= end) {
        wav_chunk_header_t* ch = (wav_chunk_header_t*)cursor;
        cursor += sizeof(wav_chunk_header_t);
        if (cursor + ch->size > end) break;

        if (tag_eq(ch->id, "fmt ") && ch->size >= sizeof(wav_fmt_t)) {
            fmt = *(wav_fmt_t*)cursor;
        } else if (tag_eq(ch->id, "data")) {
            data_ptr  = cursor;
            data_size = ch->size;
            break;
        }
        uint32_t step = ch->size;
        if (step & 1) step++;
        cursor += step;
    }

    if (!data_ptr || fmt.audio_format != 1 || fmt.bits_per_sample != 16 ||
        (fmt.num_channels != 1 && fmt.num_channels != 2)) {
        serial_puts("[audio] unsupported WAV format\n");
        kfree(buf);
        return false;
    }

    // Конвертация в 48 kHz stereo (nearest-neighbor + mono→stereo).
    uint32_t in_frames  = data_size / (fmt.num_channels * 2);
    uint32_t in_rate    = fmt.sample_rate ? fmt.sample_rate : AC97_HW_RATE;
    uint32_t out_frames = (uint32_t)(((uint64_t)in_frames * AC97_HW_RATE) / in_rate);

    if (out_frames == 0) {
        kfree(buf);
        return false;
    }

    int16_t* out = (int16_t*)kmalloc((size_t)out_frames * 2 * sizeof(int16_t));
    if (!out) {
        kfree(buf);
        return false;
    }

    int16_t* in_samples = (int16_t*)data_ptr;
    for (uint32_t f = 0; f < out_frames; f++) {
        uint32_t src = (uint32_t)(((uint64_t)f * in_rate) / AC97_HW_RATE);
        if (src >= in_frames) src = in_frames - 1;
        int16_t l, r;
        if (fmt.num_channels == 1) {
            l = r = in_samples[src];
        } else {
            l = in_samples[src * 2];
            r = in_samples[src * 2 + 1];
        }
        out[f * 2]     = l;
        out[f * 2 + 1] = r;
    }
    kfree(buf);

    size_t n = audio_play_pcm(out, (size_t)out_frames * 2, AC97_HW_RATE);
    kfree(out);

    serial_puts("[audio] played ");
    serial_puts(path);
    serial_puts("\n");
    return n > 0;
}

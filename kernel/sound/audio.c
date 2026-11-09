// ============================================================================
// audio.c — реализация подсистемы звука
// ----------------------------------------------------------------------------
// Путь от WAV-файла до динамика:
//   1. vfs_open_file(path) → читаем файл целиком в heap.
//   2. Парсим RIFF/WAVE заголовок, находим "fmt " и "data" chunk'и.
//   3. Проверяем: PCM, 16-bit, mono/stereo. Иначе отказ.
//   4. Если частота != 48000 — делаем простейший nearest-neighbor ресемплинг.
//   5. Если mono — дублируем каждый sample в L и R.
//   6. Отправляем результат в ac97_play().
//
// Ресемплер "nearest neighbor" звучит с артефактами на тональных сигналах,
// но для системных звуков (запуск, уведомления) — приемлемо. Настоящий
// линейный интерполятор потребует float — мы пока в freestanding без SSE.
// ============================================================================

#include "audio.h"
#include "../drivers/ac97.h"
#include "../fs/vfs.h"
#include "../memory/heap.h"
#include "../debug_serial.h"

#include <stddef.h>
#include <stdint.h>

#define AC97_HW_RATE 48000u

// ----------------------------------------------------------------------------
// WAV парсер (очень компактный, без внешних зависимостей).
// ----------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    char     riff[4];         // "RIFF"
    uint32_t file_size;
    char     wave[4];         // "WAVE"
} wav_riff_header_t;

typedef struct __attribute__((packed)) {
    char     id[4];
    uint32_t size;
} wav_chunk_header_t;

typedef struct __attribute__((packed)) {
    uint16_t audio_format;    // 1 = PCM
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
bool audio_init(void) {
    return ac97_init();
}

bool audio_is_ready(void) { return ac97_is_ready(); }

void audio_set_volume(uint8_t percent) { ac97_set_volume(percent); }

void audio_stop(void) { ac97_stop(); }

size_t audio_play_pcm(const int16_t* samples, size_t sample_count, uint32_t sample_rate) {
    if (!ac97_is_ready()) return 0;
    return ac97_play(samples, sample_count, sample_rate);
}

// ----------------------------------------------------------------------------
// audio_play_wav: открывает файл, парсит, проигрывает.
// ----------------------------------------------------------------------------
bool audio_play_wav(const char* path) {
    dbg_puts("[audio] play_wav: ");
    dbg_puts(path ? path : "(null)");
    dbg_puts("\n");

    if (!ac97_is_ready()) {
        dbg_puts("[audio] no hardware — skip play_wav\n");
        return false;
    }
    if (!path) return false;

    dbg_puts("[audio] step 1: vfs_open_file\n");
    int fd = vfs_open_file(path, VFS_O_RDONLY);
    if (fd < 0) {
        dbg_puts("[audio] cannot open ");
        dbg_puts(path);
        dbg_puts("\n");
        return false;
    }
    dbg_puts("[audio] step 2: fd=");
    dbg_hex((uint32_t)fd);
    dbg_puts("\n");

    int filesize = vfs_get_filesize(fd);
    dbg_puts("[audio] step 3: filesize=");
    dbg_hex((uint32_t)filesize);
    dbg_puts("\n");
    if (filesize <= (int)sizeof(wav_riff_header_t)) {
        dbg_puts("[audio] file too small\n");
        vfs_close_file(fd);
        return false;
    }

    uint8_t* buf = (uint8_t*)kmalloc((size_t)filesize);
    dbg_puts("[audio] step 4: buf=");
    dbg_hex((uint32_t)(uintptr_t)buf);
    dbg_puts("\n");
    if (!buf) {
        dbg_puts("[audio] kmalloc failed\n");
        vfs_close_file(fd);
        return false;
    }

    int nread = vfs_read_file(fd, buf, (size_t)filesize);
    vfs_close_file(fd);
    dbg_puts("[audio] step 5: nread=");
    dbg_hex((uint32_t)nread);
    dbg_puts("\n");
    if (nread != filesize) {
        dbg_puts("[audio] short read from WAV\n");
        kfree(buf);
        return false;
    }

    // --- Проверяем RIFF/WAVE ---
    wav_riff_header_t* hdr = (wav_riff_header_t*)buf;
    if (!tag_eq(hdr->riff, "RIFF") || !tag_eq(hdr->wave, "WAVE")) {
        dbg_puts("[audio] not a RIFF/WAVE\n");
        kfree(buf);
        return false;
    }
    dbg_puts("[audio] step 6: RIFF/WAVE ok\n");

    // --- Ищем chunk'и fmt/data ---
    uint8_t*  cursor    = buf + sizeof(wav_riff_header_t);
    uint8_t*  end       = buf + filesize;
    wav_fmt_t fmt       = {0};
    uint8_t*  data_ptr  = NULL;
    uint32_t  data_size = 0;

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
        // Chunk'ы выравнены по 2 байта.
        uint32_t step = ch->size;
        if (step & 1) step++;
        cursor += step;
    }

    dbg_puts("[audio] step 7: fmt ch=");
    dbg_hex(fmt.num_channels);
    dbg_puts(" bps=");
    dbg_hex(fmt.bits_per_sample);
    dbg_puts(" rate=");
    dbg_hex(fmt.sample_rate);
    dbg_puts(" data_size=");
    dbg_hex(data_size);
    dbg_puts("\n");

    if (!data_ptr || fmt.audio_format != 1 || fmt.bits_per_sample != 16 ||
        (fmt.num_channels != 1 && fmt.num_channels != 2)) {
        dbg_puts("[audio] unsupported WAV format\n");
        kfree(buf);
        return false;
    }

    // --- Конвертация в 48 kHz stereo 16-bit ---
    // Входные фреймы: data_size / (num_channels*2) = кол-во фреймов.
    uint32_t in_frames = data_size / (fmt.num_channels * 2);
    uint32_t in_rate   = fmt.sample_rate ? fmt.sample_rate : AC97_HW_RATE;

    // Размер выходного буфера во фреймах.
    // out_frames = in_frames * HW_RATE / in_rate  (целочисленное деление).
    // Для избежания переполнения используем uint64_t.
    uint32_t out_frames = (uint32_t)(((uint64_t)in_frames * AC97_HW_RATE) / in_rate);

    dbg_puts("[audio] step 8: in_frames=");
    dbg_hex(in_frames);
    dbg_puts(" out_frames=");
    dbg_hex(out_frames);
    dbg_puts("\n");

    if (out_frames == 0) {
        dbg_puts("[audio] out_frames == 0\n");
        kfree(buf);
        return false;
    }

    int16_t* out = (int16_t*)kmalloc((size_t)out_frames * 2 * sizeof(int16_t));
    dbg_puts("[audio] step 9: out_buf=");
    dbg_hex((uint32_t)(uintptr_t)out);
    dbg_puts("\n");
    if (!out) {
        dbg_puts("[audio] out_buf kmalloc failed\n");
        kfree(buf);
        return false;
    }

    int16_t* in_samples = (int16_t*)data_ptr;

    // Nearest-neighbor ресемплинг + mono→stereo.
    for (uint32_t f = 0; f < out_frames; f++) {
        uint32_t src_frame = (uint32_t)(((uint64_t)f * in_rate) / AC97_HW_RATE);
        if (src_frame >= in_frames) src_frame = in_frames - 1;
        int16_t l, r;
        if (fmt.num_channels == 1) {
            l = r = in_samples[src_frame];
        } else {
            l = in_samples[src_frame * 2];
            r = in_samples[src_frame * 2 + 1];
        }
        out[f * 2]     = l;
        out[f * 2 + 1] = r;
    }

    dbg_puts("[audio] step 10: resample done\n");

    kfree(buf);

    // Отправляем: sample_count = out_frames * 2 (L+R).
    dbg_puts("[audio] step 11: calling audio_play_pcm\n");
    size_t n = audio_play_pcm(out, (size_t)out_frames * 2, AC97_HW_RATE);
    dbg_puts("[audio] step 12: audio_play_pcm returned n=");
    dbg_hex((uint32_t)n);
    dbg_puts("\n");
    kfree(out);

    dbg_puts("[audio] played ");
    dbg_puts(path);
    dbg_puts(" (");
    dbg_hex(n);
    dbg_puts(" samples)\n");
    return n > 0;
}

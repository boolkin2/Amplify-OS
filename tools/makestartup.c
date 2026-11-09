// ============================================================================
// makestartup.c — host-утилита: генерирует WAV-файл startup.wav с приветствием
// ----------------------------------------------------------------------------
// Компиляция: gcc -O2 -o makestartup makestartup.c -lm
// Запуск:     ./makestartup initrd_content/startup.wav
//
// Генерирует 1.2 секунды 16-bit stereo 48000 Hz PCM WAV.
// Мелодия — трёхзвучный аккорд (C5 → E5 → G5 → полный) с затуханием
// через ADSR-envelope. Звучит как «ding-ding-ding» при старте ОС.
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#define SAMPLE_RATE 48000
#define DURATION    1.2   // сек
#define CHANNELS    2
#define BITS        16

// Частоты аккорда C major (C5, E5, G5).
static const double FREQS[3] = { 523.25, 659.25, 783.99 };

// ADSR-envelope (attack/decay/sustain/release).
static double envelope(double t, double duration) {
    double a = 0.01;   // attack
    double d = 0.1;    // decay
    double s = 0.6;    // sustain level
    double r = 0.4;    // release

    if (t < a) {
        return t / a;
    } else if (t < a + d) {
        return 1.0 - (1.0 - s) * (t - a) / d;
    } else if (t < duration - r) {
        return s;
    } else if (t < duration) {
        return s * (duration - t) / r;
    }
    return 0.0;
}

struct __attribute__((packed)) riff_hdr {
    char     riff[4];
    uint32_t size;
    char     wave[4];
};
struct __attribute__((packed)) fmt_chunk {
    char     id[4];
    uint32_t size;
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
};
struct __attribute__((packed)) data_chunk_hdr {
    char     id[4];
    uint32_t size;
};

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <out.wav>\n", argv[0]);
        return 1;
    }

    uint32_t total_samples = (uint32_t)(SAMPLE_RATE * DURATION);
    uint32_t data_bytes = total_samples * CHANNELS * (BITS / 8);

    FILE* f = fopen(argv[1], "wb");
    if (!f) { perror(argv[1]); return 2; }

    // RIFF
    struct riff_hdr rh = { .riff = {'R','I','F','F'}, .wave = {'W','A','V','E'} };
    rh.size = 4 + (8 + sizeof(struct fmt_chunk) - 8) + (8 + data_bytes);
    fwrite(&rh, sizeof(rh), 1, f);

    // fmt
    struct fmt_chunk fc = {
        .id = {'f','m','t',' '},
        .size = 16,
        .format = 1,
        .channels = CHANNELS,
        .sample_rate = SAMPLE_RATE,
        .byte_rate = SAMPLE_RATE * CHANNELS * (BITS / 8),
        .block_align = CHANNELS * (BITS / 8),
        .bits = BITS,
    };
    fwrite(&fc, sizeof(fc), 1, f);

    // data header
    struct data_chunk_hdr dh = { .id = {'d','a','t','a'}, .size = data_bytes };
    fwrite(&dh, sizeof(dh), 1, f);

    // data — генерируем sample по sample.
    // Первая треть — только C5, вторая — C5+E5, третья — полный аккорд.
    for (uint32_t i = 0; i < total_samples; i++) {
        double t = (double)i / SAMPLE_RATE;
        double env = envelope(t, DURATION);

        int active = 1;
        if (t > DURATION * 0.33) active = 2;
        if (t > DURATION * 0.66) active = 3;

        double s = 0.0;
        for (int k = 0; k < active; k++) {
            s += sin(2.0 * 3.14159265358979323846 * FREQS[k] * t);
        }
        s *= (env / 4.0);  // масштабируем, чтобы избежать клиппинга

        int16_t sample = (int16_t)(s * 28000.0);
        // stereo: L = R = sample (моно в стерео поток)
        fwrite(&sample, sizeof(sample), 1, f);
        fwrite(&sample, sizeof(sample), 1, f);
    }

    fclose(f);
    printf("wrote %u samples to %s (%u bytes)\n",
           total_samples, argv[1], data_bytes);
    return 0;
}

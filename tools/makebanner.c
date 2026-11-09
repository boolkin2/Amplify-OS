// ============================================================================
// makebanner.c - Host-утилита: генерация тестового PNG-баннера для MyOS
// ============================================================================
// Компилируется обычным gcc на хост-системе (НЕ кросс-компилятором).
// Использует zlib (libz) для DEFLATE-сжатия IDAT.
//
// Формат: PNG 256x64 RGBA, тип 6 (truecolor + alpha), 8 бит/канал, без interlace.
// Содержимое: диагональный сине-фиолетовый градиент + золотая рамка 2 px.
//
// Использование:
//   ./makebanner <output.png>
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <zlib.h>

#define W 256
#define H 64

// ---------- CRC32 (через zlib) ----------

static uint32_t png_crc(const uint8_t* data, size_t len) {
    return (uint32_t)crc32(0L, data, (uInt)len);
}

// ---------- Запись chunk-а ----------

static void write_u32_be(FILE* f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8),  (uint8_t)v };
    fwrite(b, 1, 4, f);
}

static void write_chunk(FILE* f, const char tag[4],
                        const uint8_t* data, size_t len)
{
    write_u32_be(f, (uint32_t)len);
    fwrite(tag, 1, 4, f);
    if (len > 0) fwrite(data, 1, len, f);

    // CRC покрывает tag + data
    uint8_t* crc_buf = (uint8_t*)malloc(4 + len);
    memcpy(crc_buf, tag, 4);
    if (len > 0) memcpy(crc_buf + 4, data, len);
    uint32_t c = png_crc(crc_buf, 4 + len);
    free(crc_buf);
    write_u32_be(f, c);
}

// ---------- Генерация пиксельных данных ----------

static void make_pixels(uint8_t* out /* W*H*4 */) {
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int r = 30  + (x * 60) / W;
            int g = 80  + (y * 80) / H;
            int b = 180 + ((W - x) * 60) / W;
            int a = 255;
            // Золотая рамка 2 px
            if (x < 2 || x >= W - 2 || y < 2 || y >= H - 2) {
                r = 255; g = 215; b = 0;
            }
            uint8_t* p = &out[(y * W + x) * 4];
            p[0] = (uint8_t)r;
            p[1] = (uint8_t)g;
            p[2] = (uint8_t)b;
            p[3] = (uint8_t)a;
        }
    }
}

// ---------- main ----------

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Ispol'zovanie: %s <output.png>\n", argv[0]);
        return 1;
    }
    const char* out_path = argv[1];

    // 1. Пиксели
    uint8_t* pixels = (uint8_t*)malloc((size_t)W * H * 4);
    if (!pixels) { fprintf(stderr, "Net pamyati\n"); return 2; }
    make_pixels(pixels);

    // 2. Подготавливаем raw-поток с filter-байтом в начале каждой строки
    size_t row = (size_t)W * 4;
    size_t raw_size = (row + 1) * H;
    uint8_t* raw = (uint8_t*)malloc(raw_size);
    if (!raw) { free(pixels); fprintf(stderr, "Net pamyati\n"); return 2; }
    for (int y = 0; y < H; y++) {
        raw[y * (row + 1)] = 0;  // filter type = None
        memcpy(&raw[y * (row + 1) + 1], &pixels[y * row], row);
    }

    // 3. Сжимаем через zlib (deflate)
    uLongf comp_bound = compressBound((uLong)raw_size);
    uint8_t* idat = (uint8_t*)malloc(comp_bound);
    if (!idat) { free(pixels); free(raw); fprintf(stderr, "Net pamyati\n"); return 2; }
    uLongf idat_len = comp_bound;
    if (compress2(idat, &idat_len, raw, (uLong)raw_size, 9) != Z_OK) {
        fprintf(stderr, "zlib compress2 failed\n");
        free(pixels); free(raw); free(idat);
        return 3;
    }

    // 4. Открываем файл и пишем PNG
    FILE* f = fopen(out_path, "wb");
    if (!f) {
        fprintf(stderr, "Ne udalos' otkryt' %s\n", out_path);
        free(pixels); free(raw); free(idat);
        return 4;
    }

    // PNG signature
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);

    // IHDR: W, H, bit_depth=8, color_type=6 (RGBA), compression=0, filter=0, interlace=0
    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(W >> 24); ihdr[1] = (uint8_t)(W >> 16);
    ihdr[2] = (uint8_t)(W >> 8);  ihdr[3] = (uint8_t)W;
    ihdr[4] = (uint8_t)(H >> 24); ihdr[5] = (uint8_t)(H >> 16);
    ihdr[6] = (uint8_t)(H >> 8);  ihdr[7] = (uint8_t)H;
    ihdr[8]  = 8;  // bit depth
    ihdr[9]  = 6;  // color type RGBA
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = 0;
    write_chunk(f, "IHDR", ihdr, sizeof(ihdr));

    // IDAT
    write_chunk(f, "IDAT", idat, idat_len);

    // IEND
    write_chunk(f, "IEND", NULL, 0);

    fclose(f);
    free(pixels);
    free(raw);
    free(idat);

    printf("[makebanner] %s (%dx%d, ~%lu B PNG)\n",
           out_path, W, H, (unsigned long)idat_len + 57);
    return 0;
}

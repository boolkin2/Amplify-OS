// ============================================================================
// ppmdiff.c — сравнение двух скриншотов QEMU (PPM P6) для headless-тестов.
// ----------------------------------------------------------------------------
//   ppmdiff a.ppm b.ppm   ->   "diff=<пикселей> bbox=<x0>,<y0>,<x1>,<y1> size=<W>x<H>"
// Нужен тестам 41.2: частичный вывод кадра и курсор с сохранением фона легко
// оставляют «следы», которых не видно ни в serial, ни в счётчиках, — их
// ловит только сравнение картинок. На C, а не на python: в WSL python3 нет.
// Код возврата: 0 — файлы прочитаны (даже если отличаются), 2 — ошибка.
// ============================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char* load(const char* path, int* w, int* h) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    char magic[3] = {0};
    int maxv = 0;
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) { fclose(f); return NULL; }
    // Комментарии (#...) между полями заголовка QEMU не пишет, но формат допускает.
    int vals[3], n = 0;
    while (n < 3) {
        int c = fgetc(f);
        if (c == EOF) { fclose(f); return NULL; }
        if (c == '#') { while (c != '\n' && c != EOF) c = fgetc(f); continue; }
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        ungetc(c, f);
        if (fscanf(f, "%d", &vals[n]) != 1) { fclose(f); return NULL; }
        n++;
    }
    fgetc(f);                                   // один пробельный байт после maxval
    *w = vals[0]; *h = vals[1]; maxv = vals[2];
    if (*w <= 0 || *h <= 0 || maxv != 255) { fclose(f); return NULL; }
    size_t bytes = (size_t)*w * (size_t)*h * 3;
    unsigned char* buf = malloc(bytes);
    if (!buf || fread(buf, 1, bytes, f) != bytes) { free(buf); fclose(f); return NULL; }
    fclose(f);
    return buf;
}

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: ppmdiff a.ppm b.ppm\n"); return 2; }
    int w1, h1, w2, h2;
    unsigned char* a = load(argv[1], &w1, &h1);
    unsigned char* b = load(argv[2], &w2, &h2);
    if (!a || !b) { fprintf(stderr, "ppmdiff: cannot read input\n"); return 2; }
    if (w1 != w2 || h1 != h2) { printf("diff=-1 size-mismatch %dx%d vs %dx%d\n", w1, h1, w2, h2); return 0; }
    long diff = 0;
    int x0 = w1, y0 = h1, x1 = -1, y1 = -1;
    for (int y = 0; y < h1; y++) {
        const unsigned char* pa = a + (size_t)y * w1 * 3;
        const unsigned char* pb = b + (size_t)y * w1 * 3;
        for (int x = 0; x < w1; x++) {
            if (pa[x*3] != pb[x*3] || pa[x*3+1] != pb[x*3+1] || pa[x*3+2] != pb[x*3+2]) {
                diff++;
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (diff == 0) printf("diff=0 bbox=- size=%dx%d\n", w1, h1);
    else printf("diff=%ld bbox=%d,%d,%d,%d size=%dx%d\n", diff, x0, y0, x1, y1, w1, h1);
    return 0;
}

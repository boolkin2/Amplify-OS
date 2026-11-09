import urllib.request
import re
import os

print('Downloading font data...')
url = 'https://raw.githubusercontent.com/torvalds/linux/master/lib/fonts/font_8x16.c'
text = urllib.request.urlopen(url).read().decode('utf-8')

# Extract all 0xXX that are followed by a comma
hex_values = re.findall(r'0x[0-9a-fA-F]{2}(?=,)', text)

if len(hex_values) == 4096:
    print('Found exactly 4096 hex values.')
else:
    print(f'Found {len(hex_values)} hex values. Extracting first 4096.')
    hex_values = hex_values[:4096]

# Let's write the generated C code
font_c_content = """// ============================================================================
// font.c - Bitmap-шрифт 8x16 для MyOS (полный ASCII / CP437 256 символов)
// ============================================================================

#include "font.h"
#include "graphics.h"
#include "framebuffer.h"

// ============================================================================
// Данные шрифта 8x16 — полный набор CP437 (256 символов)
// Каждый символ = 16 байт, каждый байт = 1 строка пикселей (бит 7 = левый)
// ============================================================================

const uint8_t font_8x16[FONT_GLYPHS * FONT_HEIGHT] = {
"""

# format nicely into 256 blocks of 16 bytes
for i in range(256):
    block = hex_values[i*16:(i+1)*16]
    font_c_content += '    ' + ', '.join(block) + ','
    if i < 255:
        font_c_content += '\n'

font_c_content += """
};

// ============================================================================
// Функции рендеринга
// ============================================================================

void font_draw_char(int32_t x, int32_t y, char c, uint32_t fg, uint32_t bg) {
    uint8_t ch = (uint8_t)c;
    const uint8_t* glyph = &font_8x16[ch * FONT_HEIGHT];
    int transparent_bg = (bg == 0xFF000000);

    for (int row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < FONT_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                gfx_put_pixel(x + col, y + row, fg);
            } else if (!transparent_bg) {
                gfx_put_pixel(x + col, y + row, bg);
            }
        }
    }
}

void font_draw_string(int32_t x, int32_t y, const char* str, uint32_t fg, uint32_t bg) {
    int32_t cx = x;
    while (*str) {
        if (*str == '\\n') {
            cx = x;
            y += FONT_HEIGHT;
        } else {
            font_draw_char(cx, y, *str, fg, bg);
            cx += FONT_WIDTH;
        }
        str++;
    }
}

void font_draw_dec(int32_t x, int32_t y, int32_t num, uint32_t fg, uint32_t bg) {
    char buf[16];
    int i = 0;
    int neg = 0;
    if (num < 0) { neg = 1; num = -num; }
    if (num == 0) { buf[i++] = '0'; }
    else {
        while (num > 0) { buf[i++] = '0' + (num % 10); num /= 10; }
    }
    if (neg) buf[i++] = '-';
    // Реверсируем и выводим
    for (int j = i - 1; j >= 0; j--) {
        font_draw_char(x, y, buf[j], fg, bg);
        x += FONT_WIDTH;
    }
}

void font_draw_hex(int32_t x, int32_t y, uint32_t num, uint32_t fg, uint32_t bg) {
    const char hex[] = "0123456789ABCDEF";
    font_draw_string(x, y, "0x", fg, bg);
    x += FONT_WIDTH * 2;
    for (int i = 28; i >= 0; i -= 4) {
        font_draw_char(x, y, hex[(num >> i) & 0xF], fg, bg);
        x += FONT_WIDTH;
    }
}

int32_t font_string_width(const char* str) {
    int32_t w = 0;
    while (*str) { w += FONT_WIDTH; str++; }
    return w;
}

int32_t font_string_height(void) {
    return FONT_HEIGHT;
}
"""

with open(os.path.join('kernel', 'gui', 'font.c'), 'w', encoding='utf-8') as f:
    f.write(font_c_content)

print('Successfully generated complete font.c')

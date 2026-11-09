// ============================================================================
// rtc.c — чтение CMOS/RTC (Motorola MC146818-совместимый) в 64-битном ядре.
// ============================================================================
// Регистровый интерфейс: Index Port 0x70, Data Port 0x71.
//   0x00 — секунды        0x02 — минуты         0x04 — часы
//   0x07 — день           0x08 — месяц          0x09 — год (2 цифры)
//   0x32 — век            0x0A — статус A (UIP) 0x0B — статус B (24h, BCD)
//
// Портирован из kernel/drivers/rtc.c — никаких 64-bit особенностей, всё
// делается через inb/outb из io.h.
// ============================================================================

#include "rtc.h"
#include "io.h"
#include <stdbool.h>

#define CMOS_INDEX        0x70
#define CMOS_DATA         0x71

#define CMOS_REG_SECOND   0x00
#define CMOS_REG_MINUTE   0x02
#define CMOS_REG_HOUR     0x04
#define CMOS_REG_DAY      0x07
#define CMOS_REG_MONTH    0x08
#define CMOS_REG_YEAR     0x09
#define CMOS_REG_CENTURY  0x32
#define CMOS_REG_STATUS_A 0x0A
#define CMOS_REG_STATUS_B 0x0B

// Чтение одного CMOS-регистра. NMI оставляем выключенным (бит 7 = 1).
static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, (uint8_t)(0x80 | reg));
    return inb(CMOS_DATA);
}

// Дождаться окончания update-цикла, чтобы прочитать согласованный набор полей.
static void wait_not_updating(void) {
    for (int i = 0; i < 1000000; i++) {
        if (!(cmos_read(CMOS_REG_STATUS_A) & 0x80)) return;
    }
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)(((v >> 4) & 0x0F) * 10 + (v & 0x0F));
}

void rtc_read(rtc_time_t* out) {
    if (!out) return;

    // Читаем дважды и сравниваем — стандартный приём защиты от того,
    // что RTC проинкрементировал поле прямо во время чтения.
    uint8_t s0, mn0, h0, d0, mo0, y0, c0;
    uint8_t s1, mn1, h1, d1, mo1, y1, c1;
    uint8_t status_b;

    do {
        wait_not_updating();
        s0  = cmos_read(CMOS_REG_SECOND);
        mn0 = cmos_read(CMOS_REG_MINUTE);
        h0  = cmos_read(CMOS_REG_HOUR);
        d0  = cmos_read(CMOS_REG_DAY);
        mo0 = cmos_read(CMOS_REG_MONTH);
        y0  = cmos_read(CMOS_REG_YEAR);
        c0  = cmos_read(CMOS_REG_CENTURY);

        wait_not_updating();
        s1  = cmos_read(CMOS_REG_SECOND);
        mn1 = cmos_read(CMOS_REG_MINUTE);
        h1  = cmos_read(CMOS_REG_HOUR);
        d1  = cmos_read(CMOS_REG_DAY);
        mo1 = cmos_read(CMOS_REG_MONTH);
        y1  = cmos_read(CMOS_REG_YEAR);
        c1  = cmos_read(CMOS_REG_CENTURY);
    } while (s0 != s1 || mn0 != mn1 || h0 != h1 ||
             d0 != d1 || mo0 != mo1 || y0 != y1 || c0 != c1);

    status_b = cmos_read(CMOS_REG_STATUS_B);

    bool is_binary = (status_b & 0x04) != 0;
    bool is_24h    = (status_b & 0x02) != 0;

    bool pm = false;
    if (!is_24h && (h0 & 0x80)) {
        pm = true;
        h0 &= 0x7F;
    }

    if (!is_binary) {
        s0  = bcd_to_bin(s0);
        mn0 = bcd_to_bin(mn0);
        h0  = bcd_to_bin(h0);
        d0  = bcd_to_bin(d0);
        mo0 = bcd_to_bin(mo0);
        y0  = bcd_to_bin(y0);
        c0  = bcd_to_bin(c0);
    }

    if (!is_24h) {
        if (h0 == 12) h0 = 0;
        if (pm) h0 = (uint8_t)(h0 + 12);
    }

    out->second = s0;
    out->minute = mn0;
    out->hour   = h0;
    out->day    = d0;
    out->month  = mo0;

    uint16_t century;
    if (c0 >= 19 && c0 <= 25) {
        century = (uint16_t)c0 * 100;
    } else {
        century = 2000;
    }
    out->year = (uint16_t)(century + y0);
}

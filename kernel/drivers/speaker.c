// ============================================================================
// speaker.c - Реализация драйвера PC Speaker для MyOS
// ============================================================================

#include "speaker.h"
#include "timer.h"
#include "../io.h"

// ============================================================================
// Константы
// ============================================================================

#define PIT_BASE_FREQ   1193182     // Базовая частота PIT (Hz)
#define SPEAKER_PORT    0x61        // Порт управления динамиком

// ============================================================================
// Реализация
// ============================================================================

// Включить динамик на заданной частоте
void speaker_on(uint32_t frequency) {
    if (frequency == 0) return;
    
    // Вычисляем делитель для PIT канала 2
    uint32_t divisor = PIT_BASE_FREQ / frequency;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    if (divisor < 1) divisor = 1;
    
    // Настраиваем PIT канал 2 (режим 3 — square wave)
    // Команда: канал 2 (бит 7-6 = 10), чтение/запись оба байта (бит 5-4 = 11),
    // режим 3 (бит 3-1 = 011), двоичный (бит 0 = 0)
    outb(PIT_COMMAND, 0xB6);    // 10 11 011 0 = 0xB6
    
    // Отправляем делитель в канал 2
    outb(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));         // Младший байт
    outb(PIT_CHANNEL2, (uint8_t)((divisor >> 8) & 0xFF));  // Старший байт
    
    // Включаем динамик: устанавливаем биты 0 и 1 порта 0x61
    // Бит 0 — подключает PIT канал 2 к Gate
    // Бит 1 — включает динамик
    uint8_t tmp = inb(SPEAKER_PORT);
    if ((tmp & 3) != 3) {
        outb(SPEAKER_PORT, tmp | 3);
    }
}

// Выключить динамик
void speaker_off(void) {
    // Сбрасываем биты 0 и 1 порта 0x61
    uint8_t tmp = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, tmp & 0xFC);
}

// Воспроизвести тон заданной частоты и длительности
void speaker_beep(uint32_t frequency, uint32_t duration_ms) {
    speaker_on(frequency);
    timer_sleep(duration_ms);
    speaker_off();
}

// Воспроизвести мелодию загрузки (мажорный аккорд C-E-G-C6)
void speaker_play_boot_melody(void) {
    speaker_beep(NOTE_C5, 80);
    timer_sleep(20);
    speaker_beep(NOTE_E5, 80);
    timer_sleep(20);
    speaker_beep(NOTE_G5, 80);
    timer_sleep(20);
    speaker_beep(NOTE_C6, 150);
}

// ============================================================================
// mouse.c - Реализация драйвера мыши (PS/2)
// ============================================================================

#include "mouse.h"
#include "../io.h"
#include "../arch/i386/isr.h"
#include "../gui/framebuffer.h"
#include "../gui/cursor.h"

// ============================================================================
// Внутренние константы и переменные
// ============================================================================

#define PS2_CMD         0x64
#define PS2_DATA        0x60

static mouse_state_t current_state = {0, 0, false, false, false};

// Буфер для 3-байтового пакета
static uint8_t mouse_cycle = 0;
static int8_t  mouse_byte[3];

// ============================================================================
// Низкоуровневые функции общения с контроллером
// ============================================================================

// Ожидание контроллера (type 0 = можно читать, type 1 = можно писать)
static inline void mouse_wait(uint8_t type) {
    int timeout = 100000;
    if (type == 0) {
        while (timeout--) {
            if (inb(PS2_CMD) & 1) return;
        }
    } else {
        while (timeout--) {
            if (!(inb(PS2_CMD) & 2)) return;
        }
    }
}

// Запись команды напрямую в мышь (через команду 0xD4 контроллера)
static inline void mouse_write(uint8_t write) {
    mouse_wait(1);
    outb(PS2_CMD, 0xD4); // Команда контроллеру: следующее слово отправить мыши
    mouse_wait(1);
    outb(PS2_DATA, write); // Отправляем байт мыши
}

// Чтение байта от мыши
static inline uint8_t mouse_read(void) {
    mouse_wait(0);
    return inb(PS2_DATA);
}

// ============================================================================
// Обработчик прерывания (IRQ12)
// ============================================================================

static uint32_t mouse_handler(registers_t* regs) {
    // Проверяем, что прерывание действительно от мыши (бит 5 status регистра)
    uint8_t status = inb(PS2_CMD);
    if (!(status & 0x20)) {
        return (uint32_t)regs; // Не от мыши (возможно, от клавиатуры)
    }

    // Читаем байт данных
    int8_t mouse_in = inb(PS2_DATA);
    
    switch (mouse_cycle) {
        case 0:
            // Первый байт пакета (должен иметь бит 3 установленным)
            if (mouse_in & 0x08) {
                mouse_byte[0] = mouse_in;
                mouse_cycle++;
            }
            break;
            
        case 1:
            // Второй байт пакета (смещение X)
            mouse_byte[1] = mouse_in;
            mouse_cycle++;
            break;
            
        case 2:
            // Третий байт пакета (смещение Y)
            mouse_byte[2] = mouse_in;
            
            // Расшифровка пакета
            bool x_neg = mouse_byte[0] & 0x10;
            bool y_neg = mouse_byte[0] & 0x20;
            bool x_overflow = mouse_byte[0] & 0x40;
            bool y_overflow = mouse_byte[0] & 0x80;
            
            // Игнорируем пакет, если было переполнение (мышь дернули слишком быстро)
            if (!x_overflow && !y_overflow) {
                // Смещение по X (с учетом знака)
                int32_t dx = mouse_byte[1];
                if (x_neg) dx |= 0xFFFFFF00;
                
                // Смещение по Y (с учетом знака)
                int32_t dy = mouse_byte[2];
                if (y_neg) dy |= 0xFFFFFF00;
                
                // Обновляем координаты
                current_state.x += dx;
                current_state.y -= dy; // В PS/2 Y растет вверх, а на экране - вниз
                
                // Ограничиваем координаты границами экрана
                if (framebuffer_is_initialized()) {
                    int32_t max_x = framebuffer_get_width() - 1;
                    int32_t max_y = framebuffer_get_height() - 1;
                    
                    if (current_state.x < 0) current_state.x = 0;
                    if (current_state.x > max_x) current_state.x = max_x;
                    
                    if (current_state.y < 0) current_state.y = 0;
                    if (current_state.y > max_y) current_state.y = max_y;
                }
            }
            
            // Обновляем состояние кнопок
            current_state.left_btn = mouse_byte[0] & 0x01;
            current_state.right_btn = mouse_byte[0] & 0x02;
            current_state.middle_btn = mouse_byte[0] & 0x04;
            
            // Готовимся к следующему пакету
            mouse_cycle = 0;
            
            // Перерисовываем курсор на новых координатах
            cursor_update(current_state.x, current_state.y);
            
            break;
    }
    
    return (uint32_t)regs;
}

// ============================================================================
// Внешний API
// ============================================================================

void mouse_init(void) {
    // 1. Включаем устройство мыши
    mouse_wait(1);
    outb(PS2_CMD, 0xA8);
    
    // 2. Включаем прерывания IRQ12 в контроллере
    mouse_wait(1);
    outb(PS2_CMD, 0x20); // Команда "читать байт конфигурации"
    mouse_wait(0);
    uint8_t status = inb(PS2_DATA);
    
    status |= 2;      // Устанавливаем бит 1 (IRQ12)
    status &= ~0x20;  // Очищаем бит 5 (отключаем clock)
    
    mouse_wait(1);
    outb(PS2_CMD, 0x60); // Команда "писать байт конфигурации"
    mouse_wait(1);
    outb(PS2_DATA, status);
    
    // 3. Устанавливаем настройки мыши по умолчанию
    mouse_write(0xF6);
    mouse_read(); // Ждем ACK
    
    // 4. Включаем передачу пакетов от мыши (Data Reporting)
    mouse_write(0xF4);
    mouse_read(); // Ждем ACK
    
    // Устанавливаем курсор в центр экрана по умолчанию
    if (framebuffer_is_initialized()) {
        current_state.x = framebuffer_get_width() / 2;
        current_state.y = framebuffer_get_height() / 2;
    }
    
    // Регистрируем обработчик IRQ12 (44)
    isr_register_handler(44, mouse_handler);
}

mouse_state_t mouse_get_state(void) {
    return current_state;
}

int32_t mouse_get_x(void) {
    return current_state.x;
}

int32_t mouse_get_y(void) {
    return current_state.y;
}

bool mouse_is_left_pressed(void) {
    return current_state.left_btn;
}

bool mouse_is_right_pressed(void) {
    return current_state.right_btn;
}

void mouse_set_position(int32_t x, int32_t y) {
    current_state.x = x;
    current_state.y = y;
}

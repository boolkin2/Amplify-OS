// ============================================================================
// keyboard.h — PS/2 клавиатурный драйвер для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Минимальный set 1 scancode parser + кольцевой буфер ASCII-символов.
// IRQ 1 → IOAPIC GSI 1 → vector 33 → keyboard_irq → push в FIFO.
// Shell поллит keyboard_get_char() в loop.
// ============================================================================
#ifndef AOS_X86_64_KEYBOARD_H
#define AOS_X86_64_KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

#define KBD_DATA_PORT      0x60
#define KBD_STATUS_PORT    0x64

// Special key codes (returned alongside ASCII).
#define KBD_KEY_NONE       0
#define KBD_KEY_BACKSPACE  '\b'
#define KBD_KEY_ENTER      '\n'
#define KBD_KEY_TAB        '\t'
#define KBD_KEY_ESC        27
#define KBD_KEY_BACKTAB    28
#define KBD_KEY_LEFT       ((char)0x80)
#define KBD_KEY_RIGHT      ((char)0x81)
#define KBD_KEY_UP         ((char)0x82)
#define KBD_KEY_DOWN       ((char)0x83)
#define KBD_KEY_SUPER_HOME ((char)0x84)
#define KBD_KEY_SUPER_PULSE ((char)0x85)
#define KBD_KEY_SUPER_TAB  ((char)0x86)
#define KBD_KEY_SUPER_BEACON ((char)0x87)
// [этап 49] Ctrl+Space — открыть/закрыть AMPLIFY MIND (mind_widget.c).
#define KBD_KEY_MIND       ((char)0x88)
// [48.2] Клавиши прокрутки. До 48.2 клавиатура их не выдавала вовсе (браузер
// ждал коды 0x11/0x12, которые никто не посылал).
#define KBD_KEY_PGUP       ((char)0x89)
#define KBD_KEY_PGDN       ((char)0x8A)
#define KBD_KEY_HOME       ((char)0x8B)
#define KBD_KEY_END        ((char)0x8C)
#define KBD_KEY_DELETE     ((char)0x8D)

void  keyboard_init(void);
// [41.2] Подать скан-код набора 1 от другого источника (virtio-клавиатура).
// Звать только из обработчика прерывания или при запрещённых прерываниях.
void keyboard_inject_scancode(uint8_t sc);

// [50.5] Перехват «сырых» скан-кодов набора 1 — нажатия И отпускания, до
// превращения в символы. Нужен окну гостя phantom: гостю требуется настоящая
// клавиатура (Ctrl, Alt, стрелки, автоповтор), а не поток ASCII.
//   sc       — скан-код (бит 7 — отпускание), префикс E0 сюда не приходит;
//   extended — перед кодом был префикс E0.
// Перехватчик зовётся в обработчике прерывания (IF=0): только быстрые
// действия. Вернул true — код «съеден», обычная обработка его не увидит.
// 0 — снять перехват.
typedef bool (*keyboard_raw_fn)(uint8_t sc, bool extended);
void  keyboard_set_raw_sink(keyboard_raw_fn fn);

bool  keyboard_has_char(void);
char  keyboard_get_char(void);          // 0 если буфер пуст
char  keyboard_wait_char(void);         // блокирующий sti+hlt loop
bool  keyboard_shift_pressed(void);
bool  keyboard_ctrl_pressed(void);
bool  keyboard_super_pressed(void);

#endif

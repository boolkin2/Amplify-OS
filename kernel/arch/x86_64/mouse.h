// ============================================================================
// mouse.h — PS/2 mouse driver для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// IRQ 12 (через slave PIC) → IOAPIC GSI 12 → vector 44.  3-байтный пакет
// PS/2: byte0 = button + sign + overflow flags, byte1 = dx, byte2 = dy.
// Ядро мап'ит изменения в координаты курсора (x, y) с учётом границ экрана.
// ============================================================================
#ifndef AOS_X86_64_MOUSE_H
#define AOS_X86_64_MOUSE_H

#include <stdint.h>
#include <stdbool.h>

#define MOUSE_BTN_LEFT    (1u << 0)
#define MOUSE_BTN_RIGHT   (1u << 1)
#define MOUSE_BTN_MIDDLE  (1u << 2)

typedef struct {
    int      x;
    int      y;
    uint8_t  buttons;       // bit 0 = left, 1 = right, 2 = middle
} mouse_state_t;

void                  mouse_init(void);
// [41.2] Указатель от virtio-input: абсолютные координаты экрана / смещения.
void mouse_inject_abs(int x, int y, uint8_t buttons);
void mouse_inject_rel(int dx, int dy, uint8_t buttons);

const mouse_state_t*  mouse_get_state(void);

// Polling-fallback: дрейнит 8042 буфер вручную (не полагаясь на IRQ).
// Безопасно вызывать из main loop -- если IRQ доставляется нормально,
// функция просто не найдёт данных и сразу вернётся.
void                  mouse_poll(void);

// События — каждый IRQ обновляет state, а главный поток поллит изменения.
// has_event возвращает true, если что-то изменилось с прошлого acknowledge.
// [48.2] Колесо (virtio-input: REL_WHEEL; у PS/2-мыши колеса нет — нужен
// протокол IntelliMouse, не реализован). Положительное значение — от себя.
void                  mouse_inject_wheel(int notches);
int                   mouse_take_wheel(void);      // забрать накопленное и обнулить
bool                  mouse_has_wheel(void);

bool                  mouse_has_event(void);
void                  mouse_clear_event(void);

// DEBUG: периодический дамп счётчиков IRQ/poll/bytes/packets в serial.
void                  mouse_debug_dump(void);

#endif

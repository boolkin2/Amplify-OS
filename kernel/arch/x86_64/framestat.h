// ============================================================================
// framestat.h — счётчики времени кадра (подэтап 41.2, «замер, а не догадки»).
// ----------------------------------------------------------------------------
// Зачем: до 41.2 единственной метрикой графики было drm.ms_in_present с
// разрешением PIT (10 мс) — им нельзя отличить кадр 3 мс от кадра 12 мс.
// Здесь время меряется TSC, а пересчёт в микросекунды делается по факту:
// за каждое секундное окно известны и прошедшие миллисекунды (таймер ядра),
// и прошедшие такты TSC, отдельная калибровка не нужна.
//
// Фазы кадра соответствуют пути пикселя на экран:
//   INPUT  — разбор мыши/клавиатуры и тик MIND в главном цикле WM;
//   SCENE  — отрисовка сцены (рабочий стол, окна, оверлеи) в RAM-буфер;
//   CURSOR — программный курсор поверх сцены;
//   COPY   — копирование RAM-буфера в линейный framebuffer (путь VESA);
//   XFER   — virtio-gpu TRANSFER_TO_HOST_2D вместе с ожиданием ответа;
//   FLUSH  — virtio-gpu RESOURCE_FLUSH вместе с ожиданием ответа;
//   FRAME  — весь render_frame целиком;
//   SLEEP  — hlt в главном цикле (доля сна = простой CPU).
//
// Вывод: команда `compstat` (последнее окно), `compstat watch` — строка
// `[fstat] ...` в serial раз в секунду. По умолчанию serial молчит: строки
// попадали бы в klog, а его читает инструмент MIND `logs` (эталоны 49.1).
// Для тестов то же включается файлом /disk/Sanctum/perf.conf с `fstat=1`.
//
// Вызывать только с BSP (главный цикл WM и то, что он зовёт): блокировок нет.
// ============================================================================
#ifndef AOS_X86_64_FRAMESTAT_H
#define AOS_X86_64_FRAMESTAT_H

#include <stdint.h>
#include <stdbool.h>

#define FS_CONF_PATH "/disk/Sanctum/perf.conf"

typedef enum {
    FS_INPUT = 0,
    FS_SCENE,
    FS_CURSOR,
    FS_COPY,
    FS_XFER,
    FS_FLUSH,
    FS_FRAME,
    FS_SLEEP,
    FS_PHASES
} fs_phase_t;

// Отчёт за последнее закрытое окно (~1 с).
typedef struct {
    uint32_t window_ms;            // фактическая длина окна
    uint32_t frames;               // кадров в окне
    uint32_t fps_x10;              // кадры/с * 10
    uint32_t wakeups;              // итераций главного цикла (пробуждений)
    uint32_t avg_us[FS_PHASES];    // среднее на кадр (SLEEP — на пробуждение)
    uint32_t max_us[FS_PHASES];    // самый долгий отдельный вызов фазы
    uint32_t busy_permille;        // 1000 - доля сна: загрузка BSP
    uint32_t px_per_frame;         // пикселей, отправленных на экран за кадр
    uint32_t spins_per_frame;      // оборотов busy-wait virtio за кадр
    uint32_t hw_cursor_moves;      // команд MOVE_CURSOR за окно (аппаратный курсор)
} fs_report_t;

// Метка времени фазы. RDTSC дешёвый и под KVM не вызывает выхода в гипервизор.
static inline uint64_t fs_now(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void fs_init(void);                        // читает perf.conf; звать с BSP после монтирования диска
// Есть ли в perf.conf строка, начинающаяся с `line` (например "hwcursor=0").
// Работает после fs_init; без файла — всегда false.
bool fs_conf_has(const char* line);
// Значение ключа: для строки `netbench=http://...` и key="netbench=" копирует
// в out всё до конца строки. false — ключа нет.
bool fs_conf_value(const char* key, char* out, int cap);
void fs_add(fs_phase_t p, uint64_t t0);    // добавить (сейчас - t0) к фазе
void fs_pixels(uint64_t n);                // сколько пикселей ушло на экран
void fs_frame_done(void);                  // конец render_frame
void fs_loop_tick(void);                   // раз за итерацию главного цикла; закрывает окно
bool fs_last_report(fs_report_t* out);     // false, пока не закрыто ни одно окно
const char* fs_phase_name(int p);          // ASCII-имя фазы для отчётов
void fs_set_serial(bool on);
bool fs_serial(void);

#endif

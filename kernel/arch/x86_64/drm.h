// ============================================================================
// drm.h — DRM-подобный уровень AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// Лёгкая надстройка над virtio_gpu, которая:
//   * Управляет lifecycle "framebuffer'а ядра" (один primary FB, привязанный
//     к doublebuf'у).
//   * Предоставляет syscall-ABI 0x140..0x148 (см. syscall.h).
//   * На этап 41 userspace может только enumerate + present (kernel-owned FB).
//     Полноценный create_dumb/map для произвольных user-resource'ов
//     запланирован в этап 44 (3D-стек).
//
// Идея: "DRM" в нашей ОС — это интерфейс между подсистемами рендеринга
// и физическим scanout'ом. WM не знает о virtio-gpu — он только знает о
// drm_present_primary(), который вернёт false если GPU не доступен.
//
// ----------------------------------------------------------------------------
// ОГРАНИЧЕНИЕ Z-ORDER В РЕЖИМЕ virtio-gpu 2D
// ----------------------------------------------------------------------------
// Спецификация этапа 41 упоминает «Появление Z-order на уровне GPU». В
// virtio-gpu 2D mode (без VirGL/3D) **аппаратной композиции Z-ordered
// слоёв на host'е нет**: команды SET_SCANOUT работают как single-slot
// target (один resource на один scanout), а ATTACH_BACKING+TRANSFER+FLUSH
// — это DMA-передача готового растра. Никаких blend/draw-call'ов в 2D
// API не предусмотрено.
//
// Поэтому Z-order у нас по-прежнему вычисляет WM в CPU (порядок отрисовки
// окон в doublebuf), а GPU получает уже скомпонованный кадр. Это
// архитектурно правильно: drm_present_primary — это thin "GPU swap",
// не композитор. Полноценный GPU-композитинг (несколько resource'ов,
// alpha-blend на стороне host) появится на этапе 44 через VirGL/3D или
// на этапе 42 (программный compositor с альфа-каналом).
// ============================================================================
#ifndef AOS_X86_64_DRM_H
#define AOS_X86_64_DRM_H

#include <stdint.h>
#include <stdbool.h>

// ----------------------------------------------------------------------------
// Свойства подсистемы.
// ----------------------------------------------------------------------------

// Активно ли GPU-ускорение (после успешного drm_init и подключения primary FB).
bool drm_active(void);

// Init: вызывается ОДИН раз после virtio_gpu_init() и doublebuf_init().
// Создаёт primary GPU resource, привязывает backing к doublebuf'у,
// делает set_scanout. Возвращает true если ок.
//
// Если virtio_gpu не готов или doublebuf не готов — функция тихо возвращает
// false и drm_active() остаётся false. В этом случае весь стек продолжает
// работать через классический VESA-путь.
bool drm_init(void);

// Презентация всего кадра (вызывается из doublebuf_swap при drm_active).
// Возвращает true если кадр улетел на GPU успешно. При false вызывающий
// должен сделать VESA-fallback.
// [41.2] Сменить разрешение (не больше исходного режима прошивки), см. drm.c.
bool drm_set_mode(uint32_t w, uint32_t h);

bool drm_present_primary(void);

// Презентация частичного rect'а (для оптимизации курсора и пр.).
bool drm_present_primary_rect(int x, int y, int w, int h);

// ----------------------------------------------------------------------------
// Информация для статистики / smoke-теста.
// ----------------------------------------------------------------------------
typedef struct {
    bool     active;
    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint64_t frames_presented;
    uint64_t frames_failed;
    uint64_t cmd_total;
    uint64_t resp_total;
    // Этап 41: измерение производительности. ms_in_present — суммарное
    // время в миллисекундах, проведённое в drm_present_primary (одна
    // выборка = TRANSFER_TO_HOST_2D + RESOURCE_FLUSH). На QEMU без -enable-kvm
    // одно present ~1-2 мс; на bare KVM/VT-x ожидается ~0.1 мс.
    // FPS = frames_presented * 1000 / ms_in_present.
    uint64_t ms_in_present;
    uint64_t first_present_ms;
    uint64_t last_present_ms;
} drm_info_t;

drm_info_t drm_get_info(void);

// Простой бенчмарк: сделать N принудительных present и распечатать FPS
// в serial. Возвращает true если GPU-путь дал ≥ 2× скорость над VESA-copy
// (на QEMU без KVM это редко достижимо, но для KVM/host работает).
// Полезно для документирования "5-10× CPU reduction".
bool drm_self_benchmark(uint32_t frames);

// Этап 41: property-based smoke test (kernel-side эквивалент userspace
// stage41_gputest). Запускается одной строкой из kernel_main после drm_init.
// Печатает в serial по строке на свойство: [drm-test] PN ...: OK|FAIL.
void drm_self_test(void);

// ----------------------------------------------------------------------------
// Syscall handlers (вызываются из syscall_dispatch).
// На этап 41 поддержаны только OPEN / GET_INFO / PRESENT / DESTROY как
// userspace-наблюдатели. Полноценные create_dumb / map / addfb /
// pageflip отложены до этапа 44.
// ----------------------------------------------------------------------------
int64_t drm_sys_open(uint64_t a1, uint64_t a2);
int64_t drm_sys_get_info(uint64_t user_ptr);
int64_t drm_sys_present(uint64_t a1, uint64_t a2);
int64_t drm_sys_create_dumb(uint64_t w, uint64_t h);
int64_t drm_sys_map(uint64_t resource_id);
int64_t drm_sys_addfb(uint64_t a1, uint64_t a2);
int64_t drm_sys_setcrtc(uint64_t a1, uint64_t a2);
int64_t drm_sys_pageflip(uint64_t a1, uint64_t a2);
int64_t drm_sys_destroy(uint64_t resource_id);

// Структура, которую userspace получает по AOS_SYS_DRM_GET_INFO.
typedef struct {
    uint32_t active;            // 0 / 1
    uint32_t width;
    uint32_t height;
    uint64_t frames_presented;
    uint64_t frames_failed;
    uint64_t cmd_total;
    uint64_t resp_total;
    uint32_t resource_id;
    uint32_t reserved;
} aos_drm_info_t;

#endif

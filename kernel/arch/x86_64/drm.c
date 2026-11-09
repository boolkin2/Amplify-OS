// ============================================================================
// drm.c — реализация DRM-подобной обёртки над virtio_gpu.
// ----------------------------------------------------------------------------
// Привязывает primary GPU resource к RAM-буферу doublebuf'а (heap, virt==phys
// благодаря identity-map нижних 4 GB) и предоставляет drm_present_primary
// для doublebuf_swap. Userspace-syscall'ы — пока только наблюдательные
// (read-only), потому что кадровая buffer'а живёт в kernel heap'е.
// ============================================================================
#include "drm.h"
#include "memory.h"
#include "virtio_gpu.h"
#include "doublebuf.h"
#include "graphics.h"
#include "fb.h"
#include "syscall.h"
#include "serial.h"
#include "timer.h"
#include "heap.h"
#include "compositor.h"
#include "mouse.h"

// ----------------------------------------------------------------------------
// Состояние.
// ----------------------------------------------------------------------------
typedef struct {
    bool     active;
    uint32_t resource_id;
    uint32_t width;
    uint32_t height;
    uint64_t frames_ok;
    uint64_t frames_fail;
    // Этап 41: учёт времени, проведённого в present (для метрики FPS).
    uint64_t ms_in_present;
    uint64_t first_present_ms;
    uint64_t last_present_ms;
} drm_state_t;

static drm_state_t s = {0};

bool drm_active(void) { return s.active; }

drm_info_t drm_get_info(void) {
    drm_info_t r;
    r.active            = s.active;
    r.resource_id       = s.resource_id;
    r.width             = s.width;
    r.height            = s.height;
    r.frames_presented  = s.frames_ok;
    r.frames_failed     = s.frames_fail;
    r.cmd_total         = virtio_gpu_cmd_count();
    r.resp_total        = virtio_gpu_resp_count();
    r.ms_in_present     = s.ms_in_present;
    r.first_present_ms  = s.first_present_ms;
    r.last_present_ms   = s.last_present_ms;
    return r;
}

// ----------------------------------------------------------------------------
// Init.
// ----------------------------------------------------------------------------
bool drm_init(void) {
    serial_puts("[drm] init begin\n");

    if (!virtio_gpu_is_ready()) {
        serial_puts("[drm] virtio-gpu not ready -> fallback VESA\n");
        return false;
    }
    const gfx_target_t* db = doublebuf_target();
    if (!db || !db->pixels) {
        serial_puts("[drm] doublebuf not ready\n");
        return false;
    }

    // Размеры берём из doublebuf — они согласованы с VESA fb.
    uint32_t w = (uint32_t)db->width;
    uint32_t h = (uint32_t)db->height;

    uint32_t res = virtio_gpu_resource_create_2d(w, h);
    if (!res) {
        serial_puts("[drm] resource_create_2d failed\n");
        return false;
    }

    uint64_t backing_phys = virt_to_phys(db->pixels);   // identity map
    uint32_t backing_size = (uint32_t)(w * h * 4u);

    if (!virtio_gpu_resource_attach_backing(res, backing_phys, backing_size)) {
        serial_puts("[drm] attach_backing failed\n");
        virtio_gpu_resource_unref(res);
        return false;
    }
    if (!virtio_gpu_set_scanout_default(res, w, h)) {
        serial_puts("[drm] set_scanout failed\n");
        virtio_gpu_resource_unref(res);
        return false;
    }

    s.active      = true;
    s.resource_id = res;
    // [41.2] К этому моменту прерывания разрешены — убеждаемся, что MSI-X
    // controlq действительно доходит (иначе откат на опрос).
    virtio_gpu_irq_selftest();
    s.width       = w;
    s.height      = h;

    serial_puts("[drm] active: resource_id=");
    serial_print_dec(res);
    serial_puts(", ");
    serial_print_dec(w); serial_puts("x"); serial_print_dec(h);
    serial_puts(" (backing 0x");
    serial_print_hex(backing_phys, 16);
    serial_puts(", ");
    serial_print_dec(backing_size);
    serial_puts(" B)\n");
    return true;
}

// ----------------------------------------------------------------------------
// [41.2] Смена режима без VBE: у virtio-gpu «режим» — это просто размер
// ресурса, назначенного на scanout. Создаём ресурс нового размера с новым
// буфером кадра, переключаем scanout на него и только потом удаляем старый —
// экран ни на миг не остаётся без картинки.
// Ограничение: не больше исходного режима прошивки (GOP). Его линейный
// framebuffer остаётся запасным путём текстовой консоли (паника), и логический
// режим крупнее него вывел бы эту консоль за границы LFB.
// Окна, открытые до смены, сохраняют прежний размер; новые создаются под новый.
// Вызывать с BSP из главного потока.
// ----------------------------------------------------------------------------
bool drm_set_mode(uint32_t w, uint32_t h) {
    if (!s.active) return false;
    if (w < 640 || h < 480 || (w & 1) || (h & 1)) return false;
    if ((int)w > doublebuf_native_width() || (int)h > doublebuf_native_height()) return false;
    if (w == s.width && h == s.height) return true;

    uint32_t* buf = (uint32_t*)kmalloc((size_t)w * h * 4);
    if (!buf) return false;
    for (size_t i = 0; i < (size_t)w * h; i++) buf[i] = 0xFF000000u;

    uint32_t res = virtio_gpu_resource_create_2d(w, h);
    if (!res) { kfree(buf); return false; }
    if (!virtio_gpu_resource_attach_backing(res, virt_to_phys(buf), w * h * 4u) ||
        !virtio_gpu_set_scanout_default(res, w, h)) {
        virtio_gpu_resource_unref(res);
        kfree(buf);
        return false;
    }

    uint32_t old_res = s.resource_id;
    s.resource_id = res;
    s.width  = w;
    s.height = h;
    uint32_t* old_buf = doublebuf_adopt(buf, (int)w, (int)h);
    virtio_gpu_resource_unref(old_res);       // старый ресурс больше не смотрит на old_buf
    kfree(old_buf);

    fb_set_logical_mode(w, h);
    compositor_resize((int)w, (int)h);
    virtio_gpu_note_mode(w, h);
    mouse_inject_rel(0, 0, mouse_get_state()->buttons);   // вернуть курсор в новые границы

    serial_puts("[drm] mode set ");
    serial_print_dec(w); serial_puts("x"); serial_print_dec(h);
    serial_puts(", resource_id="); serial_print_dec(res); serial_puts("\n");
    return true;
}

// ----------------------------------------------------------------------------
// Present.
// ----------------------------------------------------------------------------
bool drm_present_primary(void) {
    if (!s.active) return false;
    uint64_t t0 = timer_get_uptime_ms();
    bool ok = virtio_gpu_present(s.resource_id, s.width, s.height, 0);
    uint64_t t1 = timer_get_uptime_ms();
    if (ok) {
        s.frames_ok++;
        s.ms_in_present += (t1 - t0);
        if (s.first_present_ms == 0) s.first_present_ms = t0;
        s.last_present_ms = t1;
    } else {
        s.frames_fail++;
    }
    return ok;
}

bool drm_present_primary_rect(int x, int y, int w, int h) {
    if (!s.active) return false;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (w <= 0 || h <= 0) return false;
    if ((uint32_t)(x + w) > s.width)  w = (int)s.width  - x;
    if ((uint32_t)(y + h) > s.height) h = (int)s.height - y;

    virtio_gpu_rect_t r;
    r.x = (uint32_t)x; r.y = (uint32_t)y;
    r.width = (uint32_t)w; r.height = (uint32_t)h;

    uint64_t t0 = timer_get_uptime_ms();
    bool ok = virtio_gpu_present(s.resource_id, s.width, s.height, &r);
    uint64_t t1 = timer_get_uptime_ms();
    if (ok) {
        s.frames_ok++;
        s.ms_in_present += (t1 - t0);
        if (s.first_present_ms == 0) s.first_present_ms = t0;
        s.last_present_ms = t1;
    } else {
        s.frames_fail++;
    }
    return ok;
}

// ----------------------------------------------------------------------------
// Этап 41: property-based smoke test, эквивалентный userspace stage41_gputest,
// но запускаемый из ядра один раз после drm_init. Печатает [drm-test] OK/FAIL
// для каждого свойства; test_stage41_gpu.sh парсит этот вывод.
//
// Свойства (те же, что в userspace тесте — см. userspace/x86_64/stage41_gputest.c):
//   P1. drm_active() возвращает 0 или 1 (валидное булево).
//   P2. при active: width>0, height>0, resource_id>0.
//   P3. при !active: frames_presented==frames_failed==0.
//   P4. cmd_total >= resp_total.
//   P5. inflight = cmd_total - resp_total <= 256 (размер controlq).
//   P6. N принудительных present увеличивают frames_presented на N
//       и (cmd+resp)_total на 2N (TRANSFER + FLUSH).
// ----------------------------------------------------------------------------
void drm_self_test(void) {
    serial_puts("[drm-test] begin\n");
    drm_info_t before = drm_get_info();

    // P1.
    bool a = drm_active();
    serial_puts("[drm-test] P1 (active bool valid): ");
    serial_puts(((a == 0) || (a == 1)) ? "OK" : "FAIL");
    serial_puts("\n");

    // P2 / P3.
    if (before.active) {
        bool p2 = (before.width > 0) && (before.height > 0) && (before.resource_id > 0);
        serial_puts("[drm-test] P2 (active sane dims): "); serial_puts(p2 ? "OK" : "FAIL"); serial_puts("\n");
    } else {
        bool p3 = (before.frames_presented == 0) && (before.frames_failed == 0);
        serial_puts("[drm-test] P3 (inactive zero frames): "); serial_puts(p3 ? "OK" : "FAIL"); serial_puts("\n");
    }

    // P4.
    bool p4 = (before.cmd_total >= before.resp_total);
    serial_puts("[drm-test] P4 (cmd >= resp): "); serial_puts(p4 ? "OK" : "FAIL"); serial_puts("\n");

    // P5.
    uint64_t inflight = before.cmd_total - before.resp_total;
    bool p5 = (inflight <= 256);
    serial_puts("[drm-test] P5 (inflight <= 256): "); serial_puts(p5 ? "OK" : "FAIL"); serial_puts("\n");

    // P6: только при active. Делаем 4 принудительных present и сверяем счётчики.
    if (before.active) {
        const uint32_t N = 4;
        for (uint32_t i = 0; i < N; i++) (void)drm_present_primary();
        drm_info_t after = drm_get_info();
        uint64_t dframes = after.frames_presented - before.frames_presented;
        uint64_t dcmd    = after.cmd_total - before.cmd_total;
        uint64_t dresp   = after.resp_total - before.resp_total;
        // Каждый present = TRANSFER + FLUSH = 2 cmd. Ожидаем dcmd = 2N.
        bool p6 = (dframes == N) && (dcmd == 2 * N) && (dresp == 2 * N);
        serial_puts("[drm-test] P6 (4 frames -> +4 ok, +8 cmd, +8 resp): ");
        serial_puts(p6 ? "OK" : "FAIL");
        serial_puts(" (dframes="); serial_print_dec(dframes);
        serial_puts(" dcmd=");     serial_print_dec(dcmd);
        serial_puts(" dresp=");    serial_print_dec(dresp);
        serial_puts(")\n");
    }
    serial_puts("[drm-test] end\n");
}

// ----------------------------------------------------------------------------
// Self-benchmark: сравнить N принудительных present'ов через GPU и
// эквивалентный CPU-копировальный путь (qword-блок в VESA framebuffer).
// Распечатать результат в serial. Возвращает true если GPU-путь оказался
// быстрее VESA минимум вдвое.
//
// Почему сравниваем именно так: drm_present (TRANSFER_TO_HOST_2D + FLUSH)
// — это операция фиксированной стоимости для host'а (~один DMA-burst).
// VESA-копия — N×4 байт CPU-store'ов в LFB. На bare KVM/VT-x GPU должен
// уверенно опередить CPU на 1024×768. На QEMU TCG разница меньше из-за
// эмуляции, но обычно в пределах 1.5-2× в пользу GPU.
// ----------------------------------------------------------------------------
bool drm_self_benchmark(uint32_t frames) {
    if (!s.active) {
        serial_puts("[drm-bench] skipped (GPU inactive)\n");
        return false;
    }
    if (frames < 4) frames = 4;
    if (frames > 256) frames = 256;

    const gfx_target_t* db = doublebuf_target();
    const aos64_fb_t*   st = fb_state();
    if (!db || !st || !st->ok) {
        serial_puts("[drm-bench] no doublebuf/fb\n");
        return false;
    }

    // --- 1. GPU pass ---
    uint64_t gpu_t0 = timer_get_uptime_ms();
    uint32_t gpu_ok = 0;
    for (uint32_t i = 0; i < frames; i++) {
        if (drm_present_primary()) gpu_ok++;
    }
    uint64_t gpu_dt = timer_get_uptime_ms() - gpu_t0;
    if (gpu_dt == 0) gpu_dt = 1;   // защита от деления на ноль

    // --- 2. CPU/VESA pass ---
    // Делаем те же N кадров через qword-копию RAM→LFB.
    // (То же, что doublebuf_swap делает в VESA-fallback.)
    size_t qwords = ((size_t)db->height * (size_t)db->pitch) / 8;
    uint64_t* dst = (uint64_t*)st->vaddr;
    uint64_t* src = (uint64_t*)db->pixels;
    uint64_t cpu_t0 = timer_get_uptime_ms();
    for (uint32_t i = 0; i < frames; i++) {
        for (size_t k = 0; k < qwords; k++) dst[k] = src[k];
    }
    uint64_t cpu_dt = timer_get_uptime_ms() - cpu_t0;
    if (cpu_dt == 0) cpu_dt = 1;

    // --- 3. Отчёт ---
    uint64_t gpu_fps_x10 = (uint64_t)gpu_ok * 10000ULL / gpu_dt;   // FPS×10
    uint64_t cpu_fps_x10 = (uint64_t)frames * 10000ULL / cpu_dt;
    uint64_t ratio_x10   = gpu_fps_x10 * 10ULL / (cpu_fps_x10 ? cpu_fps_x10 : 1);

    serial_puts("[drm-bench] frames="); serial_print_dec(frames);
    serial_puts(" GPU: "); serial_print_dec(gpu_dt); serial_puts(" ms");
    serial_puts(" ("); serial_print_dec(gpu_fps_x10 / 10);
    serial_puts("."); serial_print_dec(gpu_fps_x10 % 10); serial_puts(" fps)");
    serial_puts(" CPU: "); serial_print_dec(cpu_dt); serial_puts(" ms");
    serial_puts(" ("); serial_print_dec(cpu_fps_x10 / 10);
    serial_puts("."); serial_print_dec(cpu_fps_x10 % 10); serial_puts(" fps)");
    serial_puts(" GPU/CPU ratio: "); serial_print_dec(ratio_x10 / 10);
    serial_puts("."); serial_print_dec(ratio_x10 % 10); serial_puts("x\n");

    return gpu_fps_x10 >= cpu_fps_x10 * 2;
}

// ============================================================================
// Syscall handlers. Все ABI: ret = syscall_dispatch (uint64_t).
// Отрицательное значение в RAX — ошибка (см. errno).
// ============================================================================

// AOS_SYS_DRM_OPEN — возвращает 1 если GPU активен, 0 если VESA-fallback.
int64_t drm_sys_open(uint64_t a1, uint64_t a2) {
    (void)a1; (void)a2;
    return s.active ? 1 : 0;
}

// AOS_SYS_DRM_GET_INFO — копирует aos_drm_info_t в user-buffer.
// На этап 41 user_ptr — это адрес в user vm_space, но т.к. у нас identity-map
// нижних 4 GB и user.ld линкует program в 0x80_0000_0000, реальный физ адрес
// добывается через walk PML4. Упрощённо: мы предполагаем, что caller передал
// валидный writable указатель в свой own vm_space. Если нет — page fault
// поймает CPL=3 и выдаст SIGSEGV (см. этап 40 sig.c).
int64_t drm_sys_get_info(uint64_t user_ptr) {
    if (user_ptr == 0) return -AOS_EFAULT;

    aos_drm_info_t info;
    info.active           = s.active ? 1u : 0u;
    info.width            = s.width;
    info.height           = s.height;
    info.frames_presented = s.frames_ok;
    info.frames_failed    = s.frames_fail;
    info.cmd_total        = virtio_gpu_cmd_count();
    info.resp_total       = virtio_gpu_resp_count();
    info.resource_id      = s.resource_id;
    info.reserved         = 0;

    // Прямая запись по виртуальному адресу — на user.ld'шных VM space'ах
    // верхние таблицы общие (kernel half), а нижние user-биты выставлены при
    // vm_map_user_page; user адреса доступны из ядра напрямую только если
    // мы в его CR3. На момент syscall_entry CR3=user, так что валидно.
    aos_drm_info_t* dst = (aos_drm_info_t*)(uintptr_t)user_ptr;
    *dst = info;
    return 0;
}

// AOS_SYS_DRM_PRESENT — попросить ядро сделать present. Полезно для тестов.
int64_t drm_sys_present(uint64_t a1, uint64_t a2) {
    (void)a1; (void)a2;
    if (!s.active) return -AOS_ENODEV;
    return drm_present_primary() ? 0 : -AOS_EIO;
}

// Остальные — заглушки до этапа 44.
int64_t drm_sys_create_dumb(uint64_t w, uint64_t h) {
    (void)w; (void)h;
    return -AOS_ENOSYS;
}
int64_t drm_sys_map(uint64_t resource_id) {
    (void)resource_id;
    return -AOS_ENOSYS;
}
int64_t drm_sys_addfb(uint64_t a1, uint64_t a2) {
    (void)a1; (void)a2;
    return -AOS_ENOSYS;
}
int64_t drm_sys_setcrtc(uint64_t a1, uint64_t a2) {
    (void)a1; (void)a2;
    return -AOS_ENOSYS;
}
int64_t drm_sys_pageflip(uint64_t a1, uint64_t a2) {
    (void)a1; (void)a2;
    return -AOS_ENOSYS;
}
int64_t drm_sys_destroy(uint64_t resource_id) {
    (void)resource_id;
    return -AOS_ENOSYS;
}

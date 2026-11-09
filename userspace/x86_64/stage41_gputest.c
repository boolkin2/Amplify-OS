// ============================================================================
// stage41_gputest.c — userspace property-based test для AOS_SYS_DRM_*.
// ----------------------------------------------------------------------------
// Это userspace ELF, лежит в initrd. Запускается из shell:
//   launch /stage41_gputest.elf
//
// Проверяемые свойства (выводятся на stdout, "OK" / "FAIL"):
//   P1.  drm_open() возвращает 0 или 1 (валидное булево).
//   P2.  drm_get_info() при active==1 даёт width>0, height>0, resource_id>0.
//   P3.  drm_get_info() при active==0 даёт frames_presented == frames_failed == 0.
//   P4.  cmd_total >= resp_total всегда (приходящие never опережают отправленные).
//   P5.  Inflight ( = cmd_total - resp_total) <= 256 (размер controlq).
//   P6.  N форсированных drm_present() увеличивают frames_presented ровно на N
//        (если active==1) и cmd/resp_total на 2N (TRANSFER + FLUSH).
//   P7.  drm_create_dumb / drm_addfb / drm_pageflip возвращают -ENOSYS на этап 41.
//
// Свойства подобраны так, что они одинаково уместны и при active GPU,
// и при VESA-fallback — это даёт ровно один тест на оба пути.
// ============================================================================
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

// Прямой доступ к syscall (libc-обвязка не делает alias для DRM-номеров).
#ifndef AOS_SYS_DRM_OPEN
#define AOS_SYS_DRM_OPEN        0x140
#define AOS_SYS_DRM_GET_INFO    0x141
#define AOS_SYS_DRM_PRESENT     0x142
#define AOS_SYS_DRM_CREATE_DUMB 0x143
#define AOS_SYS_DRM_MAP         0x144
#define AOS_SYS_DRM_ADDFB       0x145
#define AOS_SYS_DRM_SETCRTC     0x146
#define AOS_SYS_DRM_PAGEFLIP    0x147
#define AOS_SYS_DRM_DESTROY     0x148
#endif

typedef struct {
    uint32_t active;
    uint32_t width;
    uint32_t height;
    uint64_t frames_presented;
    uint64_t frames_failed;
    uint64_t cmd_total;
    uint64_t resp_total;
    uint32_t resource_id;
    uint32_t reserved;
} aos_drm_info_t;

// SysV x86_64 syscall ABI: rax=num, rdi/rsi/rdx/r10/r8/r9=args.
static inline long aos_syscall1(long n, long a) {
    long r;
    register long r10 __asm__("r10");
    (void)r10;
    __asm__ __volatile__("syscall"
                         : "=a"(r)
                         : "0"(n), "D"(a)
                         : "rcx", "r11", "memory");
    return r;
}
static inline long aos_syscall2(long n, long a, long b) {
    long r;
    __asm__ __volatile__("syscall"
                         : "=a"(r)
                         : "0"(n), "D"(a), "S"(b)
                         : "rcx", "r11", "memory");
    return r;
}

static int g_pass = 0;
static int g_fail = 0;

static void check(const char* name, int cond) {
    if (cond) {
        g_pass++;
        printf("  [OK]   %s\n", name);
    } else {
        g_fail++;
        printf("  [FAIL] %s\n", name);
    }
}

int main(void) {
    printf("=== stage41 GPU property test ===\n");

    // P1: open returns 0 or 1.
    long open_r = aos_syscall2(AOS_SYS_DRM_OPEN, 0, 0);
    check("P1: drm_open in {0,1}", open_r == 0 || open_r == 1);
    int active = (int)open_r;

    // P2/P3: get_info coherence.
    aos_drm_info_t info = {0};
    long gi = aos_syscall1(AOS_SYS_DRM_GET_INFO, (long)(uintptr_t)&info);
    check("P2: drm_get_info returns 0", gi == 0);

    if (active) {
        check("P2a: active info has width>0",  info.width  > 0);
        check("P2a: active info has height>0", info.height > 0);
        check("P2a: active info has resource_id>0", info.resource_id > 0);
    } else {
        check("P3: inactive frames_presented==0", info.frames_presented == 0);
        check("P3: inactive frames_failed==0",    info.frames_failed    == 0);
    }

    // P4: cmd >= resp (always).
    check("P4: cmd_total >= resp_total", info.cmd_total >= info.resp_total);
    // P5: inflight <= 256.
    uint64_t inflight = info.cmd_total - info.resp_total;
    check("P5: inflight <= 256", inflight <= 256);

    // P6: N drm_present calls bump counters by N (frames_presented) and 2N (cmd/resp_total).
    if (active) {
        const int N = 5;
        aos_drm_info_t before = info;
        int present_ok = 1;
        for (int i = 0; i < N; i++) {
            long pr = aos_syscall2(AOS_SYS_DRM_PRESENT, 0, 0);
            if (pr != 0) { present_ok = 0; break; }
        }
        aos_drm_info_t after = {0};
        aos_syscall1(AOS_SYS_DRM_GET_INFO, (long)(uintptr_t)&after);

        check("P6a: N presents all returned 0", present_ok);
        check("P6b: frames_presented +=N",
              after.frames_presented - before.frames_presented == (uint64_t)N);
        // Каждый present шлёт 2 команды (TRANSFER_TO_HOST_2D + RESOURCE_FLUSH).
        check("P6c: cmd_total += 2N",
              after.cmd_total - before.cmd_total == (uint64_t)(2 * N));
        check("P6d: resp_total += 2N",
              after.resp_total - before.resp_total == (uint64_t)(2 * N));
        check("P6e: cmd_total still >= resp_total", after.cmd_total >= after.resp_total);
    } else {
        // На VESA-пути drm_present должен вернуть -ENODEV.
        long pr = aos_syscall2(AOS_SYS_DRM_PRESENT, 0, 0);
        check("P6f: inactive present returns negative", pr < 0);
    }

    // P7: deferred syscalls return -ENOSYS.
    long cd = aos_syscall2(AOS_SYS_DRM_CREATE_DUMB, 64, 64);
    check("P7a: create_dumb -> -ENOSYS", cd == -38);
    long af = aos_syscall2(AOS_SYS_DRM_ADDFB, 0, 0);
    check("P7b: addfb -> -ENOSYS", af == -38);
    long pf = aos_syscall2(AOS_SYS_DRM_PAGEFLIP, 0, 0);
    check("P7c: pageflip -> -ENOSYS", pf == -38);

    printf("=== stage41 result: %d pass, %d fail ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

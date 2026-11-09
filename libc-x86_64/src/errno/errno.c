// ============================================================================
// errno.c — TLS-like errno и __aos_set_errno.
// ----------------------------------------------------------------------------
// Этап 39: процесс однопоточный (pthread — stubs), поэтому errno — обычная
// global. На этапе 45/46, когда добавим реальные потоки, заменим на
// __thread int errno или TLS-структуру через FS-base.
// ============================================================================
#include <errno.h>
#include "../syscall/aos_arch.h"

static int g_errno;

int* __errno_location(void) {
    return &g_errno;
}

// Если ret из ядра — отрицательное и помещается в диапазон 1..4095 —
// трактуем его как -errno (POSIX convention). Иначе — возвращаем как есть.
long __aos_set_errno(long ret) {
    if (ret < 0 && ret > -4096) {
        g_errno = (int)(-ret);
        return -1;
    }
    return ret;
}

// ============================================================================
// perms.c - Проверка прав доступа к файлам (Этап 32)
// ============================================================================
// Семантика — Unix:
//   - Если actor == owner    → используем биты owner  (0700)
//   - Иначе если actor.gid == file.gid → биты group   (0070)
//   - Иначе                  → биты other            (0007)
//   - root (uid==0) всегда получает любой запрошенный доступ.
// ============================================================================

#include "security.h"

bool sec_check_perm(uint32_t file_uid, uint32_t file_gid, uint32_t mode,
                    uint32_t actor_uid, uint32_t actor_gid, sec_perm_t perm) {
    // root всегда побеждает.
    if (actor_uid == SEC_UID_ROOT) return true;

    uint32_t bits;
    if (actor_uid == file_uid) {
        bits = (mode >> 6) & 0x7;
    } else if (actor_gid == file_gid) {
        bits = (mode >> 3) & 0x7;
    } else {
        bits = mode & 0x7;
    }

    return (bits & (uint32_t)perm) == (uint32_t)perm;
}

bool sec_check_perm_current(uint32_t file_uid, uint32_t file_gid,
                            uint32_t mode, sec_perm_t perm) {
    const sec_session_t* s = sec_get_session();
    // Если сессия ещё не открыта (мы внутри firstboot/login), считаем,
    // что действует «суперпользователь установки» — иначе сам мастер не
    // сможет ничего записать. Это безопасно: до показа login screen
    // пользователю ввод не отдан.
    if (!s->active) return true;

    return sec_check_perm(file_uid, file_gid, mode,
                          s->uid, s->gid, perm);
}

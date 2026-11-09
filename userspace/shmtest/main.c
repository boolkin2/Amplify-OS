// ============================================================================
// shmtest/main.c — проверка POSIX shared memory + fork (Стадия F)
// ============================================================================
// Сценарий:
//   1. Parent создаёт shm-сегмент (key=42, size=4096) с IPC_CREAT.
//   2. Parent делает shmat, получает user-VA, пишет "Hello from parent".
//   3. Parent делает fork(). Ребёнок тоже делает shmat с тем же id и
//      ЧИТАЕТ ту же строку — это доказывает, что shm даёт НАСТОЯЩУЮ
//      общую память (не COW-клон), так как после fork у процессов
//      разные vm, но страницы сегмента замаплены физически в оба.
//   4. Ребёнок модифицирует строку "Hello from child", детачится, выходит.
//   5. Parent ждёт, читает строку: видит модификацию ребёнка.
//   6. Parent детачится и удаляет сегмент (shmctl IPC_RMID).
// ============================================================================

#include "../libmyos/myos.h"

// Простой strcpy для user-space.
static void s_strcpy(char* dst, const char* src) {
    while ((*dst++ = *src++)) {}
}

int main(void) {
    myos_print("[SHM] shmtest start, pid=");
    myos_print_int((int)myos_getpid());
    myos_print("\n");

    int id = myos_shmget(42, 4096, IPC_CREAT);
    if (id <= 0) {
        myos_print("[SHM] shmget FAILED\n");
        return 1;
    }
    myos_print("[SHM] shmget id=");
    myos_print_int(id);
    myos_print("\n");

    char* p = (char*)myos_shmat(id);
    if ((uint32_t)p == (uint32_t)-1) {
        myos_print("[SHM] parent shmat FAILED\n");
        return 1;
    }

    s_strcpy(p, "Hello from parent");
    myos_print("[SHM] parent wrote: ");
    myos_print(p);
    myos_print("\n");

    uint32_t child_pid = myos_fork();
    if (child_pid == (uint32_t)-1) {
        myos_print("[SHM] fork FAILED\n");
        return 1;
    }

    if (child_pid == 0) {
        // --- CHILD ---
        char* cp = (char*)myos_shmat(id);
        if ((uint32_t)cp == (uint32_t)-1) {
            myos_print("[SHM] child shmat FAILED\n");
            myos_exit(1);
        }
        myos_print("[SHM] child sees: ");
        myos_print(cp);
        myos_print("\n");

        s_strcpy(cp, "Hello from child");
        myos_print("[SHM] child wrote its message\n");

        myos_shmdt(cp);
        myos_exit(0);
    } else {
        // --- PARENT ---
        myos_sleep(500);   // ждём, пока ребёнок обновит память
        myos_print("[SHM] parent sees after child: ");
        myos_print(p);
        myos_print("\n");

        myos_shmdt(p);
        myos_shmctl(id, IPC_RMID);
        myos_print("[SHM] parent done\n");
    }

    return 0;
}

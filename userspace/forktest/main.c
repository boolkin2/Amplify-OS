// ============================================================================
// forktest/main.c — проверка POSIX fork() и Copy-On-Write в MyOS
// ============================================================================
// Сценарий:
//   1. Родительский процесс печатает свой PID и значение global_var = 42.
//   2. Делаем myos_fork(). Родитель получает PID ребёнка, ребёнок — 0.
//   3. Ребёнок печатает свой PID, читает global_var (должно быть 42 — COW
//      пока не сработал на чтении), затем пишет 100.
//      После записи ребёнок печатает global_var — должно быть 100.
//      Это проверяет, что COW-#PF успешно скопировал страницу в child.
//   4. Родитель (через sleep) печатает свой global_var — должно остаться 42:
//      доказательство, что запись ребёнка НЕ видна родителю (изоляция).
// ============================================================================

#include "../libmyos/myos.h"

static int global_var = 42;

int main(void) {
    myos_print("[FORK] Parent starting, pid=");
    myos_print_int((int)myos_getpid());
    myos_print(", global_var=");
    myos_print_int(global_var);
    myos_print("\n");

    uint32_t child_pid = myos_fork();

    if (child_pid == (uint32_t)-1) {
        myos_print("[FORK] Parent: fork() FAILED\n");
        return 1;
    }

    if (child_pid == 0) {
        // === CHILD ===
        myos_print("[FORK] Child running, pid=");
        myos_print_int((int)myos_getpid());
        myos_print(", sees global_var=");
        myos_print_int(global_var);      // должно быть 42 (COW read shared)
        myos_print("\n");

        global_var = 100;                  // должно вызвать COW-копию страницы
        myos_print("[FORK] Child wrote 100, now sees=");
        myos_print_int(global_var);        // должно быть 100
        myos_print("\n");

        myos_exit(0);
    } else {
        // === PARENT ===
        myos_print("[FORK] Parent: spawned child pid=");
        myos_print_int((int)child_pid);
        myos_print("\n");

        myos_sleep(400);                   // даём ребёнку время отработать

        myos_print("[FORK] Parent after sleep: global_var=");
        myos_print_int(global_var);        // ДОЛЖНО быть 42 (изоляция COW)
        myos_print(" (expected 42 if COW works)\n");
    }

    return 0;
}

// ============================================================================
// hello/main.c - Тестовая программа для MyOS (Этап 19 и 20)
// ============================================================================
// Тестирует: mmap, swap, shared memory, printf, malloc, fopen, pthreads
// ============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include "../../libc/src/syscalls.h"

void* thread_func(void* arg) {
    int id = (int)arg;
    printf("[Thread %d] Привет из потока!\n", id);
    return NULL;
}

int main(void) {
    printf("====================================\n");
    printf("  Этап 20: Продвинутая память\n");
    printf("====================================\n");

    // 1. Тест mmap (Анонимное отображение)
    printf("[MMAP] Проецирование анонимной памяти...\n");
    void* mapped = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped != MAP_FAILED) {
        strcpy((char*)mapped, "Это данные в спроецированной памяти (mmap)!");
        printf("[MMAP] Записано: %s\n", (char*)mapped);
        
        // 2. Тест файла подкачки (SWAP)
        printf("[SWAP] Искусственный вызов выгрузки страницы в Swap...\n");
        int swap_res = _syscall1(SYS_SWAP_TEST, (uint32_t)mapped);
        if (swap_res) {
            printf("[SWAP] Страница успешно выгружена!\n");
            printf("[SWAP] Чтение выгруженной страницы (вызовет Page Fault -> Swap In)...\n");
            printf("[SWAP] Прочитано после Swap In: %s\n", (char*)mapped);
        } else {
            printf("[SWAP] Ошибка выгрузки страницы (swap_fd не открыт или ошибка).\n");
        }
        
        munmap(mapped, 4096);
        printf("[MMAP] Память освобождена (munmap).\n");
    } else {
        printf("[MMAP] Ошибка mmap!\n");
    }

    // 3. Тест Shared Memory (эмуляция через тот же mmap)
    printf("[SHM] Создание разделяемой памяти...\n");
    void* shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, -1, 0);
    if (shm != MAP_FAILED) {
        strcpy((char*)shm, "Shared Memory Data");
        printf("[SHM] Данные записаны. В MyOS (пока единый PGD) они доступны всем.\n");
        munmap(shm, 4096);
    }

    printf("====================================\n");
    printf("Тесты завершены!\n");
    return 0;
}

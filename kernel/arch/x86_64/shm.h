// ============================================================================
// shm.h — POSIX-like shared memory для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// На 37.5d-2: упрощённая kernel-side версия.  Сегмент = массив физ. страниц,
// удерживаемых через pmm_inc_ref.  shm_attach маппит страницы в указанный
// vm_space по адресу из range 0x9000000000+ (часть user-space PML4[1]).
//
// API совместим с POSIX SysV: shmget/shmat/shmdt/shmctl.  fork() ещё нет,
// но за счёт refcount'a будущая COW-семантика будет естественной.
// ============================================================================
#ifndef AOS_X86_64_SHM_H
#define AOS_X86_64_SHM_H

#include <stdint.h>
#include "vm.h"

#define SHM_IPC_CREAT   0x200
#define SHM_IPC_EXCL    0x400
#define SHM_IPC_PRIVATE 0
#define SHM_IPC_RMID    0

void     shm_init(void);
int      shm_get(int key, uint64_t size, int flags);
uint64_t shm_attach(vm_space_t* vm, int id);
int      shm_detach(vm_space_t* vm, uint64_t vaddr);
int      shm_ctl(int id, int cmd);

#endif

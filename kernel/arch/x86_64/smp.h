// ============================================================================
// smp.h — Symmetric Multi-Processing для AMPLIFY-OS x86_64.
// ----------------------------------------------------------------------------
// BSP отправляет каждому AP пару INIT/SIPI; AP стартует в 16-bit real mode,
// прыгает через 32-bit PM в 64-bit Long Mode (сразу с identity-mapped
// boot_pml4), забирает свой стек по слот-счётчику, вызывает ap_main64().
// ============================================================================
#ifndef AOS_X86_64_SMP_H
#define AOS_X86_64_SMP_H

#include <stdint.h>

extern volatile uint32_t smp_cpus_started;   // = 1 при старте (BSP)

void smp_init(void);

// Вызывается из smp_trampoline.asm в long mode, на стеке AP.
void ap_main64(void);

#endif

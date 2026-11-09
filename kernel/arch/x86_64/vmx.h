// ============================================================================
// vmx.h — Intel VT-x (VMX): нижний слой гипервизора «phantom» (этап 50).
// ----------------------------------------------------------------------------
// Слои этапа 50 задуманы как у KVM: этот файл — аналог kvm-intel (что умеет
// процессор, VMXON, VMCS, вход/выход), а модель устройств гостя (BIOS, UART,
// экран, диск) живёт отдельно и про VMX не знает.
//
// Подэтап 50.0 — только РАЗВЕДКА. Ничего не включается: ни CR4.VMXE, ни
// VMXON, ни запись в IA32_FEATURE_CONTROL. Мы лишь читаем CPUID и MSR и
// отвечаем на три вопроса:
//   1) виден ли VMX нашему ядру (под TCG и без вложенной виртуализации —
//      нет, и это штатный случай, а не ошибка);
//   2) разрешила ли его прошивка (IA32_FEATURE_CONTROL);
//   3) какие управляющие биты процессор позволяет ставить — от этого зависит
//      устройство следующих подэтапов (EPT, unrestricted guest для реального
//      режима BIOS, таймер вытеснения, битовые карты).
// Возможности берутся из MSR IA32_VMX_*, а не «по модели процессора»: под
// вложенным KVM набор определяет гипервизор узла, и он уже, чем у железа.
// ============================================================================
#ifndef AOS_X86_64_VMX_H
#define AOS_X86_64_VMX_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     probed;
    bool     present;          // CPUID.1:ECX[5]
    bool     fc_locked;        // IA32_FEATURE_CONTROL[0]
    bool     fc_vmxon;         // IA32_FEATURE_CONTROL[2]: VMXON вне SMX разрешён
    bool     true_ctls;        // IA32_VMX_BASIC[55]: есть MSR TRUE_*
    bool     ready;            // всё обязательное для phantom на месте
    bool     sane;             // прочитанное не противоречит само себе

    uint64_t feature_control;
    uint64_t basic;
    // Управляющие MSR: младшие 32 бита — биты, которые ОБЯЗАНЫ быть 1,
    // старшие 32 — биты, которым РАЗРЕШЕНО быть 1. Здесь лежат «действующие»
    // значения: TRUE_*, если процессор их даёт, иначе обычные.
    uint64_t pin, proc, proc2, exit, entry;
    uint64_t misc, ept_vpid, vmcs_enum, vmfunc;
    uint64_t cr0_fixed0, cr0_fixed1, cr4_fixed0, cr4_fixed1;

    uint32_t revision;         // идентификатор ревизии VMCS (пишется в область)
    uint32_t region_size;      // размер области VMXON/VMCS, байт

    int      cpus_seen;        // ядер, дошедших до разведки (BSP + AP)
    int      cpus_ok;          // из них с VMX и пригодным FEATURE_CONTROL
} vmx_caps_t;

// Куда печатать описание: serial_puts или fb_console_puts.
typedef void (*vmx_sink_t)(const char* s);

// BSP, один раз: читает все MSR возможностей. Вызывать после smp_init() —
// к этому моменту AP уже записали своё состояние (vmx_probe_this_cpu).
void vmx_probe(void);

// На КАЖДОМ ядре (AP — из ap_main64): CPUID и IA32_FEATURE_CONTROL этого
// ядра. Только вычисления над статической памятью — на AP можно.
void vmx_probe_this_cpu(int slot);

const vmx_caps_t* vmx_caps(void);

// true — процессор позволяет завести ВМ в той схеме, что выбрана для этапа 50.
bool vmx_ready(void);

// Печать разведки. full=false — короткая сводка (экран терминала);
// full=true — ещё и сырые MSR и все распознанные биты (serial, `phantom caps`).
void vmx_describe(vmx_sink_t out, const char* prefix, bool full);

// Одна строка состояния в serial — по ней судит тест (`[phantom] status …`).
void vmx_log_status(void);

// ----------------------------------------------------------------------------
// [50.1] Включение VMX. Как в KVM: на всех ядрах сразу, пока есть хоть одна
// ВМ, и выключение, когда исчезла последняя. vCPU у нас привязан к одному AP,
// но включать только его значило бы зашить этот выбор в самый нижний слой.
// CR4.VMXE ставится здесь, в C, а не в трёх точках входа ядра (CLAUDE.md
// §5.2): на процессоре без VMX этот бит зарезервирован, запись даёт #GP.
// ----------------------------------------------------------------------------
// BSP. Возвращает маску слотов, на которых VMX теперь включён (0 — нельзя).
// Ядро-дирижёр, занятое долгой задачей, пропускается: ждать его — секунды.
uint32_t vmx_enable_all(void);
void     vmx_disable_all(void);
uint32_t vmx_on_mask(void);

// Команды VMX. Признак ошибки у всех общий: CF или ZF после команды.
static inline bool vmx_vmwrite(uint64_t field, uint64_t value) {
    uint8_t bad;
    __asm__ __volatile__("vmwrite %2, %1; setna %0"
                         : "=qm"(bad) : "r"(field), "rm"(value) : "cc");
    return !bad;
}
static inline uint64_t vmx_vmread(uint64_t field) {
    uint64_t v = 0;
    __asm__ __volatile__("vmread %1, %0" : "+rm"(v) : "r"(field) : "cc");
    return v;
}
static inline bool vmx_vmclear(uint64_t vmcs_phys) {
    uint8_t bad;
    __asm__ __volatile__("vmclear %1; setna %0" : "=qm"(bad) : "m"(vmcs_phys) : "cc", "memory");
    return !bad;
}
static inline bool vmx_vmptrld(uint64_t vmcs_phys) {
    uint8_t bad;
    __asm__ __volatile__("vmptrld %1; setna %0" : "=qm"(bad) : "m"(vmcs_phys) : "cc", "memory");
    return !bad;
}
// Сброс кэша трансляций гостя на этом ядре (все контексты EPT и все VPID).
void vmx_flush_guest_tlb(void);

#endif

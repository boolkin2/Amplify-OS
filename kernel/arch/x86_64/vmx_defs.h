// ============================================================================
// vmx_defs.h — числа архитектуры VT-x: поля VMCS, управляющие биты, причины
// выхода (Intel SDM, том 3, приложение B и глава 24). Только константы.
// Вынесено из vmx.h, чтобы тот оставался описанием НАШЕГО слоя, а не
// справочником процессора.
// ============================================================================
#ifndef AOS_X86_64_VMX_DEFS_H
#define AOS_X86_64_VMX_DEFS_H

// ---- 16-битные поля ---------------------------------------------------------
#define VMCS_VPID                 0x0000
#define VMCS_G_ES_SEL             0x0800
#define VMCS_G_CS_SEL             0x0802
#define VMCS_G_SS_SEL             0x0804
#define VMCS_G_DS_SEL             0x0806
#define VMCS_G_FS_SEL             0x0808
#define VMCS_G_GS_SEL             0x080A
#define VMCS_G_LDTR_SEL           0x080C
#define VMCS_G_TR_SEL             0x080E
#define VMCS_H_ES_SEL             0x0C00
#define VMCS_H_CS_SEL             0x0C02
#define VMCS_H_SS_SEL             0x0C04
#define VMCS_H_DS_SEL             0x0C06
#define VMCS_H_FS_SEL             0x0C08
#define VMCS_H_GS_SEL             0x0C0A
#define VMCS_H_TR_SEL             0x0C0C

// ---- 64-битные поля ---------------------------------------------------------
#define VMCS_IO_BITMAP_A          0x2000
#define VMCS_IO_BITMAP_B          0x2002
#define VMCS_MSR_BITMAP           0x2004
#define VMCS_TSC_OFFSET           0x2010
#define VMCS_EPTP                 0x201A
#define VMCS_GUEST_PHYS_ADDR      0x2400      // только чтение: адрес при сбое EPT
#define VMCS_LINK_POINTER         0x2800
#define VMCS_G_DEBUGCTL           0x2802
#define VMCS_G_PAT                0x2804
#define VMCS_G_EFER               0x2806
#define VMCS_G_PDPTE0             0x280A
#define VMCS_H_PAT                0x2C00
#define VMCS_H_EFER               0x2C02

// ---- 32-битные поля ---------------------------------------------------------
#define VMCS_PIN_CTLS             0x4000
#define VMCS_PROC_CTLS            0x4002
#define VMCS_EXC_BITMAP           0x4004
#define VMCS_PF_ERR_MASK          0x4006
#define VMCS_PF_ERR_MATCH         0x4008
#define VMCS_CR3_TARGET_COUNT     0x400A
#define VMCS_EXIT_CTLS            0x400C
#define VMCS_EXIT_MSR_STORE_CNT   0x400E
#define VMCS_EXIT_MSR_LOAD_CNT    0x4010
#define VMCS_ENTRY_CTLS           0x4012
#define VMCS_ENTRY_MSR_LOAD_CNT   0x4014
#define VMCS_ENTRY_INTR_INFO      0x4016
#define VMCS_ENTRY_EXC_ERR        0x4018
#define VMCS_ENTRY_INSN_LEN       0x401A
#define VMCS_PROC2_CTLS           0x401E
#define VMCS_INSN_ERROR           0x4400      // только чтение
#define VMCS_EXIT_REASON          0x4402
#define VMCS_EXIT_INTR_INFO       0x4404
#define VMCS_EXIT_INTR_ERR        0x4406
#define VMCS_IDT_VEC_INFO         0x4408
#define VMCS_EXIT_INSN_LEN        0x440C
#define VMCS_G_ES_LIMIT           0x4800
#define VMCS_G_CS_LIMIT           0x4802
#define VMCS_G_SS_LIMIT           0x4804
#define VMCS_G_DS_LIMIT           0x4806
#define VMCS_G_FS_LIMIT           0x4808
#define VMCS_G_GS_LIMIT           0x480A
#define VMCS_G_LDTR_LIMIT         0x480C
#define VMCS_G_TR_LIMIT           0x480E
#define VMCS_G_GDTR_LIMIT         0x4810
#define VMCS_G_IDTR_LIMIT         0x4812
#define VMCS_G_ES_AR              0x4814
#define VMCS_G_CS_AR              0x4816
#define VMCS_G_SS_AR              0x4818
#define VMCS_G_DS_AR              0x481A
#define VMCS_G_FS_AR              0x481C
#define VMCS_G_GS_AR              0x481E
#define VMCS_G_LDTR_AR            0x4820
#define VMCS_G_TR_AR              0x4822
#define VMCS_G_INTR_STATE         0x4824
#define VMCS_G_ACTIVITY           0x4826
#define VMCS_G_SYSENTER_CS        0x482A
#define VMCS_PREEMPT_TIMER        0x482E
#define VMCS_H_SYSENTER_CS        0x4C00

// ---- поля естественной ширины ------------------------------------------------
#define VMCS_CR0_MASK             0x6000
#define VMCS_CR4_MASK             0x6002
#define VMCS_CR0_SHADOW           0x6004
#define VMCS_CR4_SHADOW           0x6006
#define VMCS_EXIT_QUAL            0x6400      // только чтение
#define VMCS_G_LINEAR_ADDR        0x640A
#define VMCS_G_CR0                0x6800
#define VMCS_G_CR3                0x6802
#define VMCS_G_CR4                0x6804
#define VMCS_G_ES_BASE            0x6806
#define VMCS_G_CS_BASE            0x6808
#define VMCS_G_SS_BASE            0x680A
#define VMCS_G_DS_BASE            0x680C
#define VMCS_G_FS_BASE            0x680E
#define VMCS_G_GS_BASE            0x6810
#define VMCS_G_LDTR_BASE          0x6812
#define VMCS_G_TR_BASE            0x6814
#define VMCS_G_GDTR_BASE          0x6816
#define VMCS_G_IDTR_BASE          0x6818
#define VMCS_G_DR7                0x681A
#define VMCS_G_RSP                0x681C
#define VMCS_G_RIP                0x681E
#define VMCS_G_RFLAGS             0x6820
#define VMCS_G_PENDING_DBG        0x6822
#define VMCS_G_SYSENTER_ESP       0x6824
#define VMCS_G_SYSENTER_EIP       0x6826
#define VMCS_H_CR0                0x6C00
#define VMCS_H_CR3                0x6C02
#define VMCS_H_CR4                0x6C04
#define VMCS_H_FS_BASE            0x6C06
#define VMCS_H_GS_BASE            0x6C08
#define VMCS_H_TR_BASE            0x6C0A
#define VMCS_H_GDTR_BASE          0x6C0C
#define VMCS_H_IDTR_BASE          0x6C0E
#define VMCS_H_SYSENTER_ESP       0x6C10
#define VMCS_H_SYSENTER_EIP       0x6C12
#define VMCS_H_RSP                0x6C14
#define VMCS_H_RIP                0x6C16

// ---- управляющие биты ---------------------------------------------------------
#define PIN_EXTINT_EXIT           (1u << 0)
#define PIN_NMI_EXIT              (1u << 3)
#define PIN_PREEMPT_TIMER         (1u << 6)

#define PROC_INTR_WINDOW          (1u << 2)
#define PROC_HLT_EXIT             (1u << 7)
#define PROC_MWAIT_EXIT           (1u << 10)
#define PROC_RDPMC_EXIT           (1u << 11)
#define PROC_CR8_LOAD_EXIT        (1u << 19)
#define PROC_CR8_STORE_EXIT       (1u << 20)
#define PROC_IO_EXIT              (1u << 24)
#define PROC_MONITOR_EXIT         (1u << 29)
#define PROC_SECONDARY            (1u << 31)

#define PROC2_EPT                 (1u << 1)
#define PROC2_VPID                (1u << 5)
#define PROC2_UNRESTRICTED        (1u << 7)

#define EXIT_HOST_64              (1u << 9)
#define EXIT_SAVE_PAT             (1u << 18)
#define EXIT_LOAD_PAT             (1u << 19)
#define EXIT_SAVE_EFER            (1u << 20)
#define EXIT_LOAD_EFER            (1u << 21)
#define EXIT_SAVE_PTIMER          (1u << 22)

#define ENTRY_IA32E_GUEST         (1u << 9)
#define ENTRY_LOAD_PAT            (1u << 14)
#define ENTRY_LOAD_EFER           (1u << 15)

// ---- причины выхода (младшие 16 бит поля EXIT_REASON) --------------------------
#define VMX_EXIT_EXC_NMI          0
#define VMX_EXIT_EXTINT           1
#define VMX_EXIT_TRIPLE_FAULT     2
#define VMX_EXIT_INTR_WINDOW      7
#define VMX_EXIT_TASK_SWITCH      9
#define VMX_EXIT_CPUID            10
#define VMX_EXIT_HLT              12
#define VMX_EXIT_RDPMC            15
#define VMX_EXIT_VMCALL           18
#define VMX_EXIT_VMX_FIRST        19       // VMCLEAR..VMXON: 19..27
#define VMX_EXIT_VMX_LAST         27
#define VMX_EXIT_CR_ACCESS        28
#define VMX_EXIT_IO               30
#define VMX_EXIT_RDMSR            31
#define VMX_EXIT_WRMSR            32
#define VMX_EXIT_BAD_GUEST_STATE  33
#define VMX_EXIT_BAD_MSR_LOAD     34
#define VMX_EXIT_MWAIT            36
#define VMX_EXIT_MONITOR          39
#define VMX_EXIT_EPT_VIOLATION    48
#define VMX_EXIT_EPT_MISCONFIG    49
#define VMX_EXIT_INVEPT           50
#define VMX_EXIT_PREEMPT_TIMER    52
#define VMX_EXIT_INVVPID          53
#define VMX_EXIT_XSETBV           55
#define VMX_EXIT_ENTRY_FAIL       (1u << 31)

// ---- регистры управления и MSR -------------------------------------------------
#define CR0_PE   (1ull << 0)
#define CR0_ET   (1ull << 4)
#define CR0_NE   (1ull << 5)
#define CR0_NW   (1ull << 29)
#define CR0_CD   (1ull << 30)
#define CR0_PG   (1ull << 31)
#define CR4_PAE      (1ull << 5)
#define CR4_VMXE     (1ull << 13)
#define CR4_SMXE     (1ull << 14)
#define CR4_PCIDE    (1ull << 17)
#define CR4_OSXSAVE  (1ull << 18)

#define MSR_IA32_TSC          0x010
#define MSR_IA32_SYSENTER_CS  0x174
#define MSR_IA32_SYSENTER_ESP 0x175
#define MSR_IA32_SYSENTER_EIP 0x176
#define MSR_IA32_PAT          0x277
#define MSR_IA32_EFER         0xC0000080
#define MSR_IA32_FS_BASE      0xC0000100
#define MSR_IA32_GS_BASE      0xC0000101
#define EFER_SCE  (1ull << 0)
#define EFER_LME  (1ull << 8)
#define EFER_LMA  (1ull << 10)
#define EFER_NXE  (1ull << 11)

#endif

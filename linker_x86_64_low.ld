/* ===========================================================================
 * linker_x86_64.ld — линкер-скрипт для 64-битного ядра AMPLIFY-OS (этап 37.1).
 * ---------------------------------------------------------------------------
 * Multiboot2 загружает ядро по физическому адресу 1 MB, как и Multiboot1.
 * Несмотря на то что ядро ELF64, мы линкуем его в нижние 4 GB
 * (identity-mapped boot.asm'ом). Higher-half mapping появится в этапе 37.2.
 *
 * .multiboot ОБЯЗАН лежать в первых 32 KB образа — иначе GRUB не найдёт
 * заголовок Multiboot2.
 * ===========================================================================
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

/* Этап 38: ELF e_entry указывает на _efi_start (64-bit вход для UEFI).
 * GRUB-путь использует Multiboot2 entry-address тэг (тип 3) в boot.asm,
 * который указывает на _start (32-bit). Так один и тот же kernel.bin
 * умеет грузиться обоими путями. */
ENTRY(_efi_start)

SECTIONS
{
    . = 1M;

    .multiboot : ALIGN(8)
    {
        KEEP(*(.multiboot))
    }

    .text : ALIGN(4096)
    {
        *(.text*)
    }

    .rodata : ALIGN(4096)
    {
        *(.rodata*)
    }

    .data : ALIGN(4096)
    {
        *(.data*)
    }

    /* Этап 37.4a: секция для user-mode кода, page-aligned. */
    .user_code : ALIGN(4096)
    {
        _user_code_start = .;
        *(.user_code*)
        _user_code_end = .;
    }

    .bss : ALIGN(4096)
    {
        _bss_start = .;
        *(COMMON)
        *(.bss*)
        _bss_end = .;
    }

    _kernel_end = .;

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.note*)
        *(.comment)
    }
}

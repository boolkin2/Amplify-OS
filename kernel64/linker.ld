/* ===========================================================================
 * kernel64/linker.ld — линкер-скрипт для 64-битного ядра AMPLIFY-OS.
 * ---------------------------------------------------------------------------
 * Multiboot1 загружает нас в физическую память. Мы помещаем секции
 * .multiboot и .text начиная с 1 MB (стандартный адрес для Multiboot).
 * Поскольку 32-битный boot.asm и 64-битный kmain делят одно адресное
 * пространство (identity-mapped 0..4 GB), линкуем всё в нижние 4 GB
 * как ELF64 и используем kernel code model = large/kernel.
 *
 * Точка входа — _start (32-bit Multiboot1 entry) из boot.asm.
 * ===========================================================================
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot : ALIGN(4)
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

    .bss : ALIGN(4096)
    {
        *(COMMON)
        *(.bss*)
    }

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.note*)
        *(.comment)
    }
}

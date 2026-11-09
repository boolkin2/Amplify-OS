/* user_linker.ld - Скрипт линкера для пользовательского приложения */
ENTRY(_start)

SECTIONS {
    /* Приложение загружается по адресу 0x40000000 */
    . = 0x40000000;

    .text ALIGN(4K) : {
        *(.text)
    }

    .rodata ALIGN(4K) : {
        *(.rodata)
    }

    .data ALIGN(4K) : {
        *(.data)
    }

    .bss ALIGN(4K) : {
        *(COMMON)
        *(.bss)
    }
}

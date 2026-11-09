/* Linker script для MyOS kernel */

/* Точка входа в ядро */
ENTRY(_start)

/* Секции памяти */
SECTIONS
{
    /* Ядро загружается по адресу 1MB (стандарт для Multiboot) */
    . = 1M;

    /* Секция кода */
    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)    /* Multiboot header должен быть в начале */
        *(.text)         /* Код */
    }

    /* Секция данных только для чтения */
    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)       /* Константы */
    }

    /* Секция инициализированных данных */
    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)         /* Глобальные переменные */
    }

    /* Секция неинициализированных данных (BSS) */
    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)          /* Неинициализированные данные */
    }

    /* Конец ядра */
    _kernel_end = .;
}

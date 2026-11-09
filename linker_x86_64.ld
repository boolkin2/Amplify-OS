/* ===========================================================================
 * linker_x86_64.ld — линкер-скрипт 64-битного ядра AMPLIFY-OS.
 * ---------------------------------------------------------------------------
 * [38.1.1] Ядро в ВЕРХНЕЙ половине адресов.
 *
 * У образа два адреса:
 *   VMA (виртуальный, по нему работает весь код на C) — KERNEL_VMA + смещение,
 *       верхние 2 ГБ адресного пространства (этого требует -mcmodel=kernel);
 *   LMA (адрес загрузки, поле p_paddr в заголовках ELF) — с 1 МБ, как раньше.
 *
 * GRUB (Multiboot2) кладёт сегменты ровно по LMA. Наш BOOTX64.EFI кладёт их
 * по LMA + «сдвиг» — туда, где прошивка дала место (сдвиг кратен 2 МБ; ядро
 * узнаёт его во входной заглушке и хранит в boot_kphys_delta). Привязки к
 * адресу 1 МБ и «стены» на 8 МБ (ACPI NVS у OVMF, CLAUDE.md §5.50) у пути
 * UEFI больше нет. Таблица перемещений не нужна: все абсолютные адреса в
 * коде на C — виртуальные и от места загрузки не зависят.
 *
 * Исключение — входная заглушка (.boot.*): она исполняется ДО включения
 * наших таблиц страниц, по физическому адресу, поэтому слинкована «внизу»
 * (VMA = LMA). Старая раскладка целиком — linker_x86_64_low.ld
 * (make HIGHER_HALF=0).
 *
 * .multiboot ОБЯЗАН лежать в первых 32 КБ файла — иначе GRUB не найдёт
 * заголовок Multiboot2.
 * ===========================================================================
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

/* e_entry — _efi_start (64-битный вход для нашего загрузчика), адрес
 * «нижний»: загрузчик прибавляет к нему сдвиг. GRUB идёт по тэгу entry
 * address заголовка Multiboot2 — на 32-битный _start. */
ENTRY(_efi_start)

KERNEL_VMA = 0xFFFFFFFF80000000;

SECTIONS
{
    . = 1M;
    _kernel_start_lma = .;

    .multiboot : ALIGN(8)
    {
        KEEP(*(.multiboot))
    }

    /* Входная заглушка: VMA = LMA. */
    .boot : ALIGN(16)
    {
        *(.boot.text)
        *(.boot.data)
    }

    . = ALIGN(4096);
    . += KERNEL_VMA;

    .text ALIGN(4096) : AT(ADDR(.text) - KERNEL_VMA)
    {
        *(.text*)
    }

    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        *(.rodata*)
    }

    .data ALIGN(4096) : AT(ADDR(.data) - KERNEL_VMA)
    {
        *(.data*)
    }

    /* Этап 37.4a: секция для user-mode кода, page-aligned. */
    .user_code ALIGN(4096) : AT(ADDR(.user_code) - KERNEL_VMA)
    {
        _user_code_start = .;
        *(.user_code*)
        _user_code_end = .;
    }

    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VMA)
    {
        _bss_start = .;
        *(COMMON)
        *(.bss*)
        _bss_end = .;
    }

    _kernel_end = .;
    _kernel_end_lma = . - KERNEL_VMA;

    /* «Нижние» адреса загрузочных таблиц страниц — для входной заглушки:
     * 32-битному коду нужен адрес, влезающий в 32 бита, а сами таблицы лежат
     * в .bss (верхний VMA), чтобы код на C работал с ними как с обычными
     * массивами. */
    boot_pml4_lma    = boot_pml4    - KERNEL_VMA;
    boot_pdpt_lma    = boot_pdpt    - KERNEL_VMA;
    boot_pd_lma      = boot_pd      - KERNEL_VMA;
    boot_pdpt_hi_lma = boot_pdpt_hi - KERNEL_VMA;
    boot_pd_hi_lma   = boot_pd_hi   - KERNEL_VMA;

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.note*)
        *(.comment)
    }
}

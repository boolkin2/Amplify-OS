// ============================================================================
// uefi/main.c — этап 38: настоящий UEFI-загрузчик ядра AMPLIFY-OS.
// ----------------------------------------------------------------------------
// Что делает (в отличие от 33B-PoC):
//   1) Через SimpleFileSystem открывает на ESP файлы:
//      /kernel.bin   — основной 64-битный ELF ядра (тот же, что грузит GRUB)
//      /initrd.img   — наш RAM-диск
//   2) Парсит ELF64-заголовок и program headers, для каждого PT_LOAD
//      резервирует физические страницы через AllocatePages(AllocateAddress)
//      по p_paddr и копирует данные из файла, BSS-хвост обнуляет.
//   3) Загружает initrd в первые 4 GB через AllocatePages(AllocateMaxAddress).
//   4) Опрашивает GOP — берёт линейный framebuffer.
//   5) Ищет RSDP в EFI Configuration Table (GUID ACPI 2.0 — приоритет, 1.0 —
//      fallback). Если у фирмвари их нет, ACPI просто не подключится — это
//      допустимо для отладочного запуска.
//   6) Строит синтетический mb2_info_t с тэгами basic_meminfo, framebuffer,
//      mmap, acpi, module. Дальше ExitBootServices(), переход в Long Mode уже
//      состоялся UEFI'ом, мы просто jmp на _efi_start ядра, передавая в RDI
//      указатель на mb2_info_t.
//
// Главное преимущество: kernel_main64 НЕ ЗНАЕТ, что мы загрузились через
// UEFI — он работает с обычным MB2-info, тот же код, что и в GRUB-пути.
// ============================================================================

#include <efi.h>
#include <efilib.h>

// ---------------------------------------------------------------------------
// GUIDs, которые понадобятся.
// ---------------------------------------------------------------------------
static EFI_GUID gop_guid       = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
static EFI_GUID sfs_guid       = SIMPLE_FILE_SYSTEM_PROTOCOL;
static EFI_GUID li_guid        = LOADED_IMAGE_PROTOCOL;
static EFI_GUID acpi_20_guid   = ACPI_20_TABLE_GUID;   // {8868E871-...} — XSDP (RSDP v2+)
static EFI_GUID acpi_10_guid   = ACPI_TABLE_GUID;      // {EB9D2D30-...} — RSDP (v1)

// ---------------------------------------------------------------------------
// Глобалы (упрощают код).
// ---------------------------------------------------------------------------
static EFI_SYSTEM_TABLE*               g_st = NULL;
static EFI_SIMPLE_TEXT_OUT_PROTOCOL*   g_co = NULL;
static EFI_BOOT_SERVICES*              g_bs = NULL;
static EFI_HANDLE                      g_image = NULL;

// ---------------------------------------------------------------------------
// ELF64 структуры (минимум, нужный для PT_LOAD-парсинга).
// ---------------------------------------------------------------------------
#define ELF_MAGIC0    0x7F
#define ELF_MAGIC1    'E'
#define ELF_MAGIC2    'L'
#define ELF_MAGIC3    'F'
#define ELFCLASS64    2
#define ELFDATA2LSB   1
#define EM_X86_64     62
#define PT_LOAD       1

typedef struct {
    UINT8  e_ident[16];
    UINT16 e_type;
    UINT16 e_machine;
    UINT32 e_version;
    UINT64 e_entry;
    UINT64 e_phoff;
    UINT64 e_shoff;
    UINT32 e_flags;
    UINT16 e_ehsize;
    UINT16 e_phentsize;
    UINT16 e_phnum;
    UINT16 e_shentsize;
    UINT16 e_shnum;
    UINT16 e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    UINT32 p_type;
    UINT32 p_flags;
    UINT64 p_offset;
    UINT64 p_vaddr;
    UINT64 p_paddr;
    UINT64 p_filesz;
    UINT64 p_memsz;
    UINT64 p_align;
} __attribute__((packed)) elf64_phdr_t;

// ---------------------------------------------------------------------------
// Структуры Multiboot2 — повторяют kernel/arch/x86_64/multiboot2.h.
// Мы строим эту инфо-блок руками в памяти.
// ---------------------------------------------------------------------------
#define MB2_BOOT_MAGIC          0x36D76289u
#define MB2_TAG_END             0
#define MB2_TAG_CMDLINE         1
#define MB2_TAG_MODULE          3
#define MB2_TAG_BASIC_MEMINFO   4
#define MB2_TAG_MMAP            6
#define MB2_TAG_FRAMEBUFFER     8
#define MB2_TAG_ACPI_OLD        14
#define MB2_TAG_ACPI_NEW        15

// ---------------------------------------------------------------------------
// Утилиты вывода (на этапе до ExitBootServices работают через ConOut).
// ---------------------------------------------------------------------------
static void out(const CHAR16* s) {
    uefi_call_wrapper(g_co->OutputString, 2, g_co, (CHAR16*)s);
}

static void u64_to_hex(UINT64 v, CHAR16* buf, int digits) {
    static const CHAR16 hex[] = L"0123456789ABCDEF";
    buf[0] = L'0'; buf[1] = L'x';
    for (int i = 0; i < digits; i++)
        buf[2 + i] = hex[(v >> ((digits - 1 - i) * 4)) & 0xF];
    buf[2 + digits] = 0;
}

static void u64_to_dec(UINT64 v, CHAR16* buf) {
    CHAR16 tmp[32]; int i = 0;
    if (v == 0) { buf[0] = L'0'; buf[1] = 0; return; }
    while (v > 0 && i < 31) { tmp[i++] = L'0' + (v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

static void print_hex(const CHAR16* key, UINT64 v, int digits) {
    CHAR16 buf[24];
    u64_to_hex(v, buf, digits);
    out(key); out(buf); out(L"\r\n");
}

static void print_dec(const CHAR16* key, UINT64 v) {
    CHAR16 buf[32];
    u64_to_dec(v, buf);
    out(key); out(buf); out(L"\r\n");
}

// ---------------------------------------------------------------------------
// COM1 (0x3F8) — прямой serial debug ПОСЛЕ ExitBootServices().
// ----------------------------------------------------------------------------
// ВАЖНО: НЕ ВЫЗЫВАТЬ com1_init() до EBS!  UART до EBS принадлежит OVMF
// (TerminalDxe мирорит ConOut в Serial, BdsDxe пишет DEBUG-логи).  Если мы
// перенастроим 16550 (LCR/divisor) до EBS, состояние OVMF-драйвера UART
// рассинхронится и ВСЕ последующие OutputString пойдут в COM1 как мусор
// (0x80..0xFF байты, что мы и наблюдали).  Поэтому пока живы EFI services —
// пишем ТОЛЬКО через g_co->OutputString().  После EBS — UART наш, com1_*
// работает напрямую.
// ---------------------------------------------------------------------------
static inline void outb(UINT16 port, UINT8 val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline UINT8 inb(UINT16 port) {
    UINT8 v; __asm__ __volatile__("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}

static void com1_init(void) {
    // Те же настройки, что serial_init() в kernel/arch/x86_64/serial.c —
    // чтобы переход loader→kernel не приводил к рассинхронизации UART.
    outb(0x3F8 + 1, 0x00);    // disable IRQs
    outb(0x3F8 + 3, 0x80);    // DLAB on
    outb(0x3F8 + 0, 0x03);    // divisor lo = 3 (38400 baud)
    outb(0x3F8 + 1, 0x00);    // divisor hi
    outb(0x3F8 + 3, 0x03);    // 8N1, DLAB off
    outb(0x3F8 + 2, 0xC7);    // FIFO enable, очистить, порог 14 байт
    outb(0x3F8 + 4, 0x0B);    // IRQs enabled, RTS/DSR set
}
static void com1_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0) { }
    outb(0x3F8, (UINT8)c);
}
static void com1_puts(const char* s) {
    while (*s) {
        if (*s == '\n') com1_putc('\r');
        com1_putc(*s++);
    }
}
static void com1_puthex(UINT64 v) {
    static const char hex[] = "0123456789ABCDEF";
    com1_puts("0x");
    for (int i = 15; i >= 0; i--) com1_putc(hex[(v >> (i*4)) & 0xF]);
}

// ---------------------------------------------------------------------------
// Память: простая «куча» нашего загрузчика не нужна — все большие буферы
// берём AllocatePool/AllocatePages напрямую.
// ---------------------------------------------------------------------------
static void* uefi_pool_alloc(UINTN size) {
    void* p = NULL;
    EFI_STATUS s = uefi_call_wrapper(g_bs->AllocatePool, 3, EfiLoaderData, size, &p);
    if (EFI_ERROR(s)) return NULL;
    return p;
}

// [49.1] Буфер строго ниже 4 ГБ. Ядро до pmm_init видит только identity-map
// первых 4 ГБ (boot.asm), а AllocatePool на машине с большим объёмом RAM
// вправе вернуть адрес выше — тогда чтение инфоблока в kernel_main64
// кончилось бы исключением на ранней загрузке.
//
// [41.2] ...и строго ВНЕ кучи ядра. Куча — фиксированный диапазон
// KHEAP_START..KHEAP_END (16..144 МБ, kernel/arch/x86_64/memory.h): ядро
// размечает его, не глядя в карту памяти. AllocateMaxAddress обязан вернуть
// лишь «что-то ниже предела» — OVMF из Debian отдаёт верх RAM, а OVMF узла
// Proxmox при загрузке с CD вернул 0x01600000. initrd оказался внутри кучи,
// doublebuf (kmalloc 8 МБ) затёр его заголовки значением 0xFF000000, и
// установщик падал с Page Fault в initrd_node_read. То же грозит инфоблоку
// MB2. Поэтому: диапазон кучи заранее занимается (reserve_kernel_heap_range),
// а каждый выданный адрес ещё и проверяется — попавший в кучу блок остаётся
// занятым (чтобы прошивка не вернула его снова) и берётся следующий.
#define KHEAP_RANGE_START  (16ULL  * 1024 * 1024)
#define KHEAP_RANGE_END    (144ULL * 1024 * 1024)

static EFI_STATUS uefi_alloc_pages_low(UINTN pages, EFI_PHYSICAL_ADDRESS* out) {
    for (int attempt = 0; attempt < 128; attempt++) {
        EFI_PHYSICAL_ADDRESS addr = 0xFFFFFFFFULL;
        EFI_STATUS s = uefi_call_wrapper(g_bs->AllocatePages, 4,
            AllocateMaxAddress, EfiLoaderData, pages, &addr);
        if (EFI_ERROR(s)) return s;
        UINT64 end = (UINT64)addr + (UINT64)pages * 4096;
        if (end <= KHEAP_RANGE_START || (UINT64)addr >= KHEAP_RANGE_END) {
            *out = addr;
            return EFI_SUCCESS;
        }
    }
    return EFI_OUT_OF_RESOURCES;
}

// Занять весь диапазон кучи ядра до первых «настоящих» выделений. Идём
// кусками по 2 МБ; кусок, в котором прошивка уже что-то держит, добираем
// постранично (ошибки игнорируем: занятая страница нам и так не достанется).
// Память никому не возвращаем: после ExitBootServices она — куча ядра.
static void reserve_kernel_heap_range(void) {
    const UINTN chunk_pages = 512;                     // 2 МБ
    for (UINT64 a = KHEAP_RANGE_START; a < KHEAP_RANGE_END; a += chunk_pages * 4096) {
        EFI_PHYSICAL_ADDRESS addr = a;
        EFI_STATUS s = uefi_call_wrapper(g_bs->AllocatePages, 4,
            AllocateAddress, EfiLoaderData, chunk_pages, &addr);
        if (!EFI_ERROR(s)) continue;
        for (UINTN p = 0; p < chunk_pages; p++) {
            addr = a + (UINT64)p * 4096;
            uefi_call_wrapper(g_bs->AllocatePages, 4,
                AllocateAddress, EfiLoaderData, 1, &addr);
        }
    }
}

static void* uefi_low_alloc(UINTN size) {
    EFI_PHYSICAL_ADDRESS addr = 0;
    UINTN pages = (size + 4095) / 4096;
    if (EFI_ERROR(uefi_alloc_pages_low(pages, &addr))) return NULL;
    return (void*)(UINTN)addr;
}

static void mem_zero(void* dst, UINTN n) {
    UINT8* d = (UINT8*)dst;
    for (UINTN i = 0; i < n; i++) d[i] = 0;
}

static void mem_copy(void* dst, const void* src, UINTN n) {
    UINT8* d = (UINT8*)dst; const UINT8* s = (const UINT8*)src;
    for (UINTN i = 0; i < n; i++) d[i] = s[i];
}

static int mem_cmp(const void* a, const void* b, UINTN n) {
    const UINT8* x = (const UINT8*)a; const UINT8* y = (const UINT8*)b;
    for (UINTN i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}

// ---------------------------------------------------------------------------
// Открытие файла на ESP через LoadedImage → SimpleFileSystem.
// Возвращает указатель на буфер EfiLoaderData (caller должен FreePool/освободить
// или просто оставить как есть — после EBS память наша). out_size — размер.
// ---------------------------------------------------------------------------
static EFI_STATUS read_esp_file(const CHAR16* path, void** out_buf, UINTN* out_size) {
    EFI_STATUS s;
    EFI_LOADED_IMAGE* li = NULL;
    s = uefi_call_wrapper(g_bs->HandleProtocol, 3, g_image, &li_guid, (void**)&li);
    if (EFI_ERROR(s) || !li) return s;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* sfs = NULL;
    s = uefi_call_wrapper(g_bs->HandleProtocol, 3, li->DeviceHandle, &sfs_guid, (void**)&sfs);
    if (EFI_ERROR(s) || !sfs) return s;

    EFI_FILE* root = NULL;
    s = uefi_call_wrapper(sfs->OpenVolume, 2, sfs, &root);
    if (EFI_ERROR(s) || !root) return s;

    EFI_FILE* fh = NULL;
    s = uefi_call_wrapper(root->Open, 5, root, &fh, (CHAR16*)path,
                          EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(s) || !fh) {
        uefi_call_wrapper(root->Close, 1, root);
        return s;
    }

    // Получаем размер через FileInfo (структура EFI_FILE_INFO).
    UINTN  info_sz = 0;
    EFI_GUID fi_guid = EFI_FILE_INFO_ID;
    s = uefi_call_wrapper(fh->GetInfo, 4, fh, &fi_guid, &info_sz, NULL);
    if (s != EFI_BUFFER_TOO_SMALL) {
        uefi_call_wrapper(fh->Close, 1, fh);
        uefi_call_wrapper(root->Close, 1, root);
        return s;
    }
    EFI_FILE_INFO* fi = (EFI_FILE_INFO*)uefi_pool_alloc(info_sz);
    if (!fi) {
        uefi_call_wrapper(fh->Close, 1, fh);
        uefi_call_wrapper(root->Close, 1, root);
        return EFI_OUT_OF_RESOURCES;
    }
    s = uefi_call_wrapper(fh->GetInfo, 4, fh, &fi_guid, &info_sz, fi);
    if (EFI_ERROR(s)) {
        uefi_call_wrapper(g_bs->FreePool, 1, fi);
        uefi_call_wrapper(fh->Close, 1, fh);
        uefi_call_wrapper(root->Close, 1, root);
        return s;
    }
    UINT64 file_size = fi->FileSize;
    uefi_call_wrapper(g_bs->FreePool, 1, fi);

    void* buf = uefi_pool_alloc((UINTN)file_size);
    if (!buf) {
        uefi_call_wrapper(fh->Close, 1, fh);
        uefi_call_wrapper(root->Close, 1, root);
        return EFI_OUT_OF_RESOURCES;
    }
    UINTN read_sz = (UINTN)file_size;
    s = uefi_call_wrapper(fh->Read, 3, fh, &read_sz, buf);
    uefi_call_wrapper(fh->Close, 1, fh);
    uefi_call_wrapper(root->Close, 1, root);
    if (EFI_ERROR(s)) {
        uefi_call_wrapper(g_bs->FreePool, 1, buf);
        return s;
    }

    *out_buf  = buf;
    *out_size = (UINTN)file_size;
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Загрузка ELF64-ядра в физическую память.
// Возвращает физ. адрес entry point (e_entry поля). Размещает по p_paddr
// каждого PT_LOAD через AllocatePages(AllocateAddress).
// ---------------------------------------------------------------------------
static EFI_STATUS load_kernel_elf(const void* image, UINTN image_size,
                                  UINT64* out_entry, UINT64* out_top) {
    if (image_size < sizeof(elf64_ehdr_t)) return EFI_LOAD_ERROR;
    const elf64_ehdr_t* eh = (const elf64_ehdr_t*)image;

    if (eh->e_ident[0] != ELF_MAGIC0 || eh->e_ident[1] != ELF_MAGIC1 ||
        eh->e_ident[2] != ELF_MAGIC2 || eh->e_ident[3] != ELF_MAGIC3) {
        out(L"  ELF: bad magic\r\n");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) {
        out(L"  ELF: not 64-bit LE\r\n");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_machine != EM_X86_64) {
        out(L"  ELF: not x86_64\r\n");
        return EFI_LOAD_ERROR;
    }
    if (eh->e_phnum == 0 || eh->e_phoff == 0) {
        out(L"  ELF: no program headers\r\n");
        return EFI_LOAD_ERROR;
    }

    print_hex(L"  ELF entry: ", eh->e_entry, 16);

    // [38.1.1] Образ кладём ОДНИМ куском со сдвигом: сегмент с адресом
    // загрузки p_paddr ложится на p_paddr + delta. Ядро слинковано в верхней
    // половине адресов и о сдвиге узнаёт само (boot.asm), поэтому место
    // может быть любым — прежняя привязка к p_paddr упиралась в ACPI NVS
    // прошивки на 0x800000 (CLAUDE.md §5.50).
    //
    // Требования ядра к сдвигу: кратен 2 МБ (образ отображается страницами
    // по 2 МБ) и весь образ ниже 4 ГБ (CR3 для ядер AP грузится 32-битным
    // кодом; тождественное отображение при входе — первые 4 ГБ).
    UINT64  lo = ~0ULL, hi = 0;
    BOOLEAN high_half = FALSE;
    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t* ph = (const elf64_phdr_t*)
            ((const UINT8*)image + eh->e_phoff + (UINT64)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_paddr < lo) lo = ph->p_paddr;
        if (ph->p_paddr + ph->p_memsz > hi) hi = ph->p_paddr + ph->p_memsz;
        if (ph->p_vaddr != ph->p_paddr) high_half = TRUE;
    }
    if (hi <= lo) { out(L"  ELF: no loadable segments\r\n"); return EFI_LOAD_ERROR; }
    lo &= ~0xFFFULL;
    hi  = (hi + 0xFFFULL) & ~0xFFFULL;
    const UINT64 M2 = 0x200000ULL;

    UINT64  delta  = 0;
    BOOLEAN forced = FALSE;

    if (!high_half) {
        // Ядро старой раскладки (make HIGHER_HALF=0): виртуальный адрес равен
        // адресу загрузки, двигать его нельзя — кладём ровно по p_paddr.
        out(L"  ELF: legacy low kernel, loading at its link address\r\n");
        forced = TRUE;
    } else {
        // \kload.at на ESP — «положи образ со сдвигом N» (шестнадцатеричное
        // число). Нужен тестам 38.1: проверить, что ядро грузится с разных
        // физических адресов и что занятый адрес даёт внятный отказ.
        void* fbuf = NULL;
        UINTN fsz  = 0;
        if (!EFI_ERROR(read_esp_file(L"\\kload.at", &fbuf, &fsz)) && fbuf) {
            const UINT8* t = (const UINT8*)fbuf;
            UINT64 v = 0; UINTN k = 0; BOOLEAN any = FALSE;
            if (fsz >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) k = 2;
            for (; k < fsz; k++) {
                UINT8 c = t[k]; UINT64 d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
                else break;
                v = (v << 4) | d; any = TRUE;
            }
            uefi_call_wrapper(g_bs->FreePool, 1, fbuf);
            if (any) {
                print_hex(L"  kload.at: requested delta ", v, 16);
                if (v & (M2 - 1)) {
                    out(L"  FATAL: kload.at is not a multiple of 2 MB\r\n");
                    return EFI_INVALID_PARAMETER;
                }
                if (v + hi > 0x100000000ULL) {
                    out(L"  FATAL: kload.at puts the kernel above 4 GB\r\n");
                    return EFI_INVALID_PARAMETER;
                }
                if (v + lo < KHEAP_RANGE_END && v + hi > KHEAP_RANGE_START) {
                    out(L"  FATAL: kload.at puts the kernel into the kernel heap range\r\n");
                    return EFI_INVALID_PARAMETER;
                }
                delta = v; forced = TRUE;
            }
        }
    }

    UINTN pages = (UINTN)((hi - lo) / 0x1000);
    if (forced) {
        EFI_PHYSICAL_ADDRESS addr = delta + lo;
        EFI_STATUS s = uefi_call_wrapper(g_bs->AllocatePages, 4,
            AllocateAddress, EfiLoaderData, pages, &addr);
        if (EFI_ERROR(s)) {
            out(L"  FATAL: the requested place for the kernel is not free\r\n");
            print_hex(L"  from ", delta + lo, 16);
            print_hex(L"  to   ", delta + hi, 16);
            return s;
        }
    } else {
        // Берём с запасом в 2 МБ и выравниваем сдвиг вверх: начало образа
        // (delta + lo) не раньше выданного адреса, конец — в пределах куска.
        UINTN extra = (UINTN)(M2 / 0x1000);
        EFI_PHYSICAL_ADDRESS addr = 0;
        EFI_STATUS s = uefi_alloc_pages_low(pages + extra, &addr);
        if (EFI_ERROR(s)) {
            out(L"  FATAL: no room for the kernel below 4 GB\r\n");
            return s;
        }
        delta = ((UINT64)addr - lo + (M2 - 1)) & ~(M2 - 1);
        if ((UINT64)addr < lo) {
            // Прошивка выдала место ниже адреса линковки (не бывает: lo = 1 МБ,
            // а ниже него таких кусков нет) — сдвиг был бы отрицательным.
            out(L"  FATAL: kernel placement below its link address\r\n");
            return EFI_LOAD_ERROR;
        }
    }

    // Очищаем весь образ (хвост .bss и промежутки между сегментами).
    mem_zero((void*)(uintptr_t)(delta + lo), (UINTN)(hi - lo));

    for (UINT16 i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t* ph = (const elf64_phdr_t*)
            ((const UINT8*)image + eh->e_phoff + (UINT64)i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0) continue;
        if (ph->p_filesz > 0) {
            mem_copy((void*)(uintptr_t)(ph->p_paddr + delta),
                     (const UINT8*)image + ph->p_offset,
                     (UINTN)ph->p_filesz);
        }
    }

    print_hex(L"  kernel phys = ", delta + lo, 16);
    print_hex(L"  delta       = ", delta, 16);

    // e_entry у ядра верхней раскладки — «нижний» адрес входной заглушки
    // (она слинкована с VMA = LMA), поэтому к нему тоже прибавляется сдвиг.
    *out_entry = eh->e_entry + delta;
    *out_top   = delta + hi;
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Загрузка initrd: AllocatePages(AllocateMaxAddress=0xFFFFFFFF), copy.
// MB2 module-тэг требует, чтобы mod_start/mod_end были 32-битными.
// ---------------------------------------------------------------------------
static EFI_STATUS load_initrd(const void* image, UINTN image_size,
                              UINT64* out_start) {
    UINTN pages = (image_size + 0xFFF) / 0x1000;
    EFI_PHYSICAL_ADDRESS addr = 0;
    // [41.2] Ниже 4 ГБ и вне кучи ядра — см. uefi_alloc_pages_low.
    EFI_STATUS s = uefi_alloc_pages_low(pages, &addr);
    if (EFI_ERROR(s)) return s;

    mem_copy((void*)(uintptr_t)addr, image, image_size);
    *out_start = addr;
    return EFI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Поиск RSDP в EFI Configuration Table.
// Приоритет — ACPI 2.0+ (XSDP, GUID 8868E871), fallback — ACPI 1.0 (RSDP).
// Возвращает указатель на сам RSDP (NULL если нет).
// rsdp_is_v2 — true если найден XSDP.
// ---------------------------------------------------------------------------
static const void* find_rsdp(int* rsdp_is_v2) {
    *rsdp_is_v2 = 0;
    for (UINTN i = 0; i < g_st->NumberOfTableEntries; i++) {
        EFI_GUID* guid = &g_st->ConfigurationTable[i].VendorGuid;
        if (mem_cmp(guid, &acpi_20_guid, sizeof(EFI_GUID)) == 0) {
            *rsdp_is_v2 = 1;
            return g_st->ConfigurationTable[i].VendorTable;
        }
    }
    for (UINTN i = 0; i < g_st->NumberOfTableEntries; i++) {
        EFI_GUID* guid = &g_st->ConfigurationTable[i].VendorGuid;
        if (mem_cmp(guid, &acpi_10_guid, sizeof(EFI_GUID)) == 0) {
            return g_st->ConfigurationTable[i].VendorTable;
        }
    }
    return NULL;
}

// ---------------------------------------------------------------------------
// Опрос GOP. Возвращает 0 — успех.
// ---------------------------------------------------------------------------
typedef struct {
    UINT64  addr;
    UINT32  pitch;
    UINT32  width;
    UINT32  height;
    UINT8   bpp;
} fb_info_t;

static int query_gop(fb_info_t* fb) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    EFI_STATUS s = uefi_call_wrapper(g_bs->LocateProtocol, 3,
                                     &gop_guid, NULL, (void**)&gop);
    if (EFI_ERROR(s) || !gop) return -1;

    // [HOTFIX 20.05.2026 FullHD] Хотим 1920x1080x32 RGB. Если такого
    // режима нет в GOP (на старых машинах) — `best_diff` выберет ближайший,
    // и fb->width/height возьмут реальные значения. doublebuf/compositor/WM
    // динамически адаптируются.
    UINTN want_w = 1920, want_h = 1080;
    UINTN picked = gop->Mode->Mode;
    UINTN best_diff = 0xFFFFFFFFULL;
    for (UINT32 i = 0; i < gop->Mode->MaxMode; i++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info;
        UINTN sz = 0;
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, i, &sz, &info)))
            continue;
        if (info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor ||
            info->PixelFormat == PixelRedGreenBlueReserved8BitPerColor) {
            UINTN dw = (info->HorizontalResolution > want_w) ?
                       (info->HorizontalResolution - want_w) :
                       (want_w - info->HorizontalResolution);
            UINTN dh = (info->VerticalResolution > want_h) ?
                       (info->VerticalResolution - want_h) :
                       (want_h - info->VerticalResolution);
            UINTN diff = dw * dw + dh * dh;
            if (diff < best_diff) { best_diff = diff; picked = i; }
        }
    }
    uefi_call_wrapper(gop->SetMode, 2, gop, picked);

    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* mi = gop->Mode->Info;
    fb->addr   = gop->Mode->FrameBufferBase;
    fb->pitch  = mi->PixelsPerScanLine * 4;
    fb->width  = mi->HorizontalResolution;
    fb->height = mi->VerticalResolution;
    fb->bpp    = 32;
    return 0;
}

// ---------------------------------------------------------------------------
// Конвертация EFI memory type → MB2 mmap type.
//   1 = available, 2 = reserved, 3 = ACPI reclaimable, 4 = ACPI NVS, 5 = bad
// ---------------------------------------------------------------------------
static UINT32 efi_to_mb2_type(UINT32 efi_type) {
    switch (efi_type) {
        case EfiLoaderCode:
        case EfiLoaderData:
        case EfiBootServicesCode:
        case EfiBootServicesData:
        case EfiConventionalMemory:
            return 1;
        case EfiACPIReclaimMemory:
            return 3;
        case EfiACPIMemoryNVS:
            return 4;
        case EfiUnusableMemory:
            return 5;
        default:
            return 2;
    }
}

// ---------------------------------------------------------------------------
// Построение синтетического mb2_info_t. Возвращает указатель на буфер.
// ВАЖНО: после ExitBootServices мы не можем вызывать AllocatePool. Поэтому
// весь буфер заранее берём с запасом (16 KB).
// ---------------------------------------------------------------------------
typedef struct {
    UINT64 initrd_start;
    UINT64 initrd_size;
    fb_info_t fb;
    const void* rsdp;
    int     rsdp_is_v2;
    UINTN   rsdp_size;
    const char* cmdline;   // 0, если не нужен CMDLINE-тег (live-режим)
} build_ctx_t;

// [41.2] 64 КБ вместо 16: запись карты памяти не проверяет границу буфера
// (24 байта на дескриптор), а резервирование кучи ядра постранично
// (reserve_kernel_heap_range) на «дырявой» карте прошивки добавляет записей.
#define MB2_INFO_BUF_SIZE   65536

static UINTN write_tag_basic_meminfo(UINT8* p, UINT64 total_ram_bytes) {
    // type=4, size=16, mem_lower (KB), mem_upper (KB)
    UINT32* w = (UINT32*)p;
    w[0] = MB2_TAG_BASIC_MEMINFO;
    w[1] = 16;
    w[2] = 640;                                    // legacy lower
    UINT64 upper_kb = total_ram_bytes / 1024;
    if (upper_kb > 0xFFFFFFFFu) upper_kb = 0xFFFFFFFFu;
    w[3] = (UINT32)upper_kb;
    return 16;
}

static UINTN write_tag_framebuffer(UINT8* p, const fb_info_t* fb) {
    // type=8, size=зависит от color_info. Для RGB type=1 пишем минимум:
    //   header (8) + addr(8) + pitch(4) + width(4) + height(4) + bpp(1)
    //   + fb_type(1) + reserved(2) + color_info_rgb (6) = 38 → align 8 = 40
    typedef struct __attribute__((packed)) {
        UINT32 type;
        UINT32 size;
        UINT64 fb_addr;
        UINT32 fb_pitch;
        UINT32 fb_width;
        UINT32 fb_height;
        UINT8  fb_bpp;
        UINT8  fb_type;
        UINT16 reserved;
        UINT8  rfp; UINT8 rms;
        UINT8  gfp; UINT8 gms;
        UINT8  bfp; UINT8 bms;
    } t_fb;
    t_fb* t = (t_fb*)p;
    UINTN sz = sizeof(t_fb);
    t->type   = MB2_TAG_FRAMEBUFFER;
    t->size   = (UINT32)sz;
    t->fb_addr = fb->addr;
    t->fb_pitch = fb->pitch;
    t->fb_width = fb->width;
    t->fb_height = fb->height;
    t->fb_bpp = fb->bpp;
    t->fb_type = 1;       // RGB
    t->reserved = 0;
    // BGRA-формат GOP даёт: B=0,G=8,R=16. Это совпадает с тем, как пишет VESA.
    t->rfp = 16; t->rms = 8;
    t->gfp = 8;  t->gms = 8;
    t->bfp = 0;  t->bms = 8;
    return (sz + 7) & ~7u;
}

static UINTN write_tag_cmdline(UINT8* p, const char* cmdline) {
    // type=1 (MB2_TAG_CMDLINE): header (8) + null-terminated string.
    UINT32* w = (UINT32*)p;
    UINTN   n = 0;
    while (cmdline[n]) n++;
    UINTN sz = 8 + n + 1;
    w[0] = MB2_TAG_CMDLINE;
    w[1] = (UINT32)sz;
    for (UINTN i = 0; i < n; i++) p[8 + i] = cmdline[i];
    p[8 + n] = 0;
    return (sz + 7) & ~7u;
}

static UINTN write_tag_module(UINT8* p, UINT64 start, UINT64 size,
                              const char* cmdline) {
    // type=3, size=variable, mod_start (u32), mod_end (u32), cmdline (string)
    typedef struct __attribute__((packed)) {
        UINT32 type;
        UINT32 size;
        UINT32 mod_start;
        UINT32 mod_end;
        char   cmdline[64];
    } t_mod;
    t_mod* t = (t_mod*)p;
    UINTN cl_len = 0;
    while (cmdline[cl_len] && cl_len < 63) cl_len++;
    UINTN sz = 8 + 4 + 4 + cl_len + 1;
    t->type = MB2_TAG_MODULE;
    t->size = (UINT32)sz;
    t->mod_start = (UINT32)start;
    t->mod_end   = (UINT32)(start + size);
    for (UINTN i = 0; i < cl_len; i++) t->cmdline[i] = cmdline[i];
    t->cmdline[cl_len] = 0;
    return (sz + 7) & ~7u;
}

static UINTN write_tag_acpi(UINT8* p, const void* rsdp, int is_v2,
                            UINTN rsdp_size) {
    // type=14 (RSDP v1, 20 байт) или 15 (XSDP v2, 36 байт)
    UINT32* w = (UINT32*)p;
    w[0] = is_v2 ? MB2_TAG_ACPI_NEW : MB2_TAG_ACPI_OLD;
    UINTN sz = 8 + rsdp_size;
    w[1] = (UINT32)sz;
    mem_copy(p + 8, rsdp, rsdp_size);
    return (sz + 7) & ~7u;
}

static UINTN write_tag_end(UINT8* p) {
    UINT32* w = (UINT32*)p;
    w[0] = MB2_TAG_END;
    w[1] = 8;
    return 8;
}

// Пишет mmap-тэг (type=6) на основе EFI memory map.
// efi_map — буфер от GetMemoryMap, count = размер/desc_size.
static UINTN write_tag_mmap(UINT8* p, const void* efi_map, UINTN map_size,
                            UINTN desc_size, UINT64* out_total) {
    typedef struct __attribute__((packed)) {
        UINT64 addr;
        UINT64 len;
        UINT32 type;
        UINT32 zero;
    } mb2_entry;
    UINT32* hdr = (UINT32*)p;
    hdr[0] = MB2_TAG_MMAP;
    // hdr[1] = размер, заполним в конце
    hdr[2] = 24;          // entry_size
    hdr[3] = 0;           // entry_version

    UINTN count = map_size / desc_size;
    mb2_entry* e = (mb2_entry*)(p + 16);
    UINT64 total = 0;
    UINTN out_cnt = 0;
    for (UINTN i = 0; i < count; i++) {
        const EFI_MEMORY_DESCRIPTOR* d = (const EFI_MEMORY_DESCRIPTOR*)
            ((const UINT8*)efi_map + i * desc_size);
        UINT64 len = d->NumberOfPages * 4096ULL;
        if (len == 0) continue;
        UINT32 t = efi_to_mb2_type(d->Type);
        if (t == 1) total += len;
        e[out_cnt].addr = d->PhysicalStart;
        e[out_cnt].len  = len;
        e[out_cnt].type = t;
        e[out_cnt].zero = 0;
        out_cnt++;
    }
    UINTN tag_sz = 16 + out_cnt * 24;
    hdr[1] = (UINT32)tag_sz;
    if (out_total) *out_total = total;
    return (tag_sz + 7) & ~7u;
}

// ---------------------------------------------------------------------------
// Сам инфоблок: basic_meminfo+cmdline+framebuffer+acpi+module → mmap+end.
// Mmap пишем самым последним, потому что его размер зависит от того,
// СКОЛЬКО раз GetMemoryMap-ExitBootServices loop понадобится.
// ---------------------------------------------------------------------------
static UINT8* build_mb2_info_partial(const build_ctx_t* ctx, UINTN* out_used) {
    UINT8* buf = (UINT8*)uefi_low_alloc(MB2_INFO_BUF_SIZE);
    if (!buf) return NULL;
    mem_zero(buf, MB2_INFO_BUF_SIZE);

    UINTN off = 8;  // место под total_size + reserved (заполним в конце)
    // basic_meminfo ОБЯЗАН идти первым, по смещению 8: цикл ExitBootServices
    // в efi_main дописывает в него mem_upper по фиксированному адресу
    // info+8.  Любой тэг перед ним был бы затёрт (так ломался cmdline).
    off += write_tag_basic_meminfo(buf + off, 0); // ram count придёт после mmap
    off = (off + 7) & ~7u;
    if (ctx->cmdline) {
        off += write_tag_cmdline(buf + off, ctx->cmdline);
        off = (off + 7) & ~7u;
    }
    off += write_tag_framebuffer(buf + off, &ctx->fb);
    off = (off + 7) & ~7u;
    if (ctx->rsdp) {
        off += write_tag_acpi(buf + off, ctx->rsdp, ctx->rsdp_is_v2,
                              ctx->rsdp_size);
        off = (off + 7) & ~7u;
    }
    if (ctx->initrd_start && ctx->initrd_size) {
        off += write_tag_module(buf + off, ctx->initrd_start, ctx->initrd_size,
                                "amplify-initrd");
        off = (off + 7) & ~7u;
    }
    *out_used = off;
    return buf;
}

// ---------------------------------------------------------------------------
// [38.1] Карта памяти прошивки — в консоль (OVMF зеркалит её в serial).
//
// До 38.1 о том, что занято у прошивки, мы знали только по отказу
// AllocatePages(AllocateAddress) на адресе 0x800000. Чтобы решать, куда
// класть ядро, нужна сама карта. Печатается только если на ESP лежит файл
// \memmap.flag: на текстовой консоли поверх кадрового буфера сотня строк —
// это секунды загрузки, обычному запуску они ни к чему.
//
// Соседние записи одного типа склеиваются (у OVMF карта дроблёная). Зовётся
// ДО наших собственных выделений, чтобы было видно именно состояние прошивки.
// Строка: «MMAP <тип> <начало>-<конец>», тип — номер EFI_MEMORY_TYPE
// (7 = свободно, 1/2 = код/данные загрузчика, 3/4 = службы загрузки,
// 5/6 = службы времени исполнения, 9/10 = ACPI, 0 = резерв, 11/12 = MMIO).
// ---------------------------------------------------------------------------
static void dump_memory_map(void) {
    UINTN  map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    EFI_STATUS s = uefi_call_wrapper(g_bs->GetMemoryMap, 5,
        &map_size, NULL, &map_key, &desc_size, &desc_ver);
    if (s != EFI_BUFFER_TOO_SMALL || !desc_size) return;
    map_size += desc_size * 16;         // выделение буфера само дробит карту
    UINT8* map = (UINT8*)uefi_pool_alloc(map_size);
    if (!map) return;
    s = uefi_call_wrapper(g_bs->GetMemoryMap, 5,
        &map_size, (EFI_MEMORY_DESCRIPTOR*)map, &map_key, &desc_size, &desc_ver);
    if (EFI_ERROR(s)) { uefi_call_wrapper(g_bs->FreePool, 1, map); return; }

    out(L"MMAP begin (firmware memory map, merged)\r\n");
    UINT64 run_start = 0, run_end = 0;
    UINT32 run_type = 0xFFFFFFFFu;
    for (UINTN off = 0; off <= map_size; off += desc_size) {
        // Последний проход (off == map_size) только допечатывает хвост.
        EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)(map + off);
        BOOLEAN last = off + desc_size > map_size;
        UINT64 st = last ? 0 : d->PhysicalStart;
        UINT64 en = last ? 0 : st + d->NumberOfPages * 4096ULL;
        if (!last && d->Type == run_type && st == run_end) { run_end = en; continue; }
        if (run_type != 0xFFFFFFFFu) {
            CHAR16 b[24];
            out(L"MMAP ");
            u64_to_dec(run_type, b);       out(b); out(L" ");
            u64_to_hex(run_start, b, 12);  out(b); out(L"-");
            u64_to_hex(run_end, b, 12);    out(b); out(L"\r\n");
        }
        if (last) break;
        run_type = d->Type; run_start = st; run_end = en;
    }
    out(L"MMAP end\r\n");
    uefi_call_wrapper(g_bs->FreePool, 1, map);
}

// ---------------------------------------------------------------------------
// Точка входа EFI.
// ---------------------------------------------------------------------------
EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE* SystemTable) {
    InitializeLib(ImageHandle, SystemTable);
    g_st    = SystemTable;
    g_co    = SystemTable->ConOut;
    g_bs    = SystemTable->BootServices;
    g_image = ImageHandle;

    // НЕ ТРОГАЕМ UART до ExitBootServices() — он принадлежит OVMF.  Логи до
    // EBS идут через OutputString (OVMF TerminalDxe сам корректно отзеркалит
    // их на serial-консоль).

    uefi_call_wrapper(g_co->ClearScreen, 1, g_co);
    uefi_call_wrapper(g_co->SetAttribute, 2, g_co,
                      EFI_TEXT_ATTR(EFI_LIGHTCYAN, EFI_BLACK));
    out(L"================================================================\r\n");
    out(L"        AMPLIFY-OS  UEFI Loader  (stage 38)\r\n");
    out(L"================================================================\r\n");
    uefi_call_wrapper(g_co->SetAttribute, 2, g_co,
                      EFI_TEXT_ATTR(EFI_LIGHTGRAY, EFI_BLACK));

    // [38.1] Карта памяти прошивки — по просьбе (файл \memmap.flag на ESP).
    {
        void* fbuf = NULL;
        UINTN fsz  = 0;
        if (!EFI_ERROR(read_esp_file(L"\\memmap.flag", &fbuf, &fsz))) {
            if (fbuf) uefi_call_wrapper(g_bs->FreePool, 1, fbuf);
            dump_memory_map();
        }
    }

    // --- Читаем kernel.bin ---------------------------------------------------
    out(L"\r\n[1/6] Reading \\kernel.bin from ESP...\r\n");
    void*  kbuf  = NULL;
    UINTN  ksize = 0;
    EFI_STATUS s = read_esp_file(L"\\kernel.bin", &kbuf, &ksize);
    if (EFI_ERROR(s)) {
        print_hex(L"   FATAL: read failed, status = ", s, 8);
        for(;;) __asm__("hlt");
    }
    print_dec(L"   size = ", ksize);

    // --- Парсим ELF и грузим в физпамять ------------------------------------
    out(L"[2/6] Loading kernel ELF segments...\r\n");
    UINT64 k_entry = 0, k_top = 0;
    s = load_kernel_elf(kbuf, ksize, &k_entry, &k_top);
    if (EFI_ERROR(s)) {
        out(L"   FATAL: ELF load failed\r\n");
        for(;;) __asm__("hlt");
    }
    print_hex(L"   entry = ", k_entry, 16);
    print_hex(L"   top   = ", k_top, 16);
    uefi_call_wrapper(g_bs->FreePool, 1, kbuf);  // оригинальный файл больше не нужен

    // [41.2] Куча ядра (16..144 МБ) — до initrd и инфоблока MB2, чтобы
    // прошивка не разместила их там (см. uefi_alloc_pages_low).
    reserve_kernel_heap_range();

    // --- Читаем initrd.img --------------------------------------------------
    out(L"[3/6] Reading \\initrd.img from ESP...\r\n");
    void*  ibuf = NULL;
    UINTN  isize = 0;
    UINT64 initrd_phys = 0;
    s = read_esp_file(L"\\initrd.img", &ibuf, &isize);
    if (EFI_ERROR(s)) {
        out(L"   WARN: initrd missing, kernel will boot without it\r\n");
    } else {
        print_dec(L"   size = ", isize);
        s = load_initrd(ibuf, isize, &initrd_phys);
        if (EFI_ERROR(s)) {
            out(L"   FATAL: initrd AllocatePages failed\r\n");
            for(;;) __asm__("hlt");
        }
        print_hex(L"   loaded @ ", initrd_phys, 16);
        uefi_call_wrapper(g_bs->FreePool, 1, ibuf);
    }

    // --- Framebuffer ---------------------------------------------------------
    out(L"[4/6] Querying GOP framebuffer...\r\n");
    build_ctx_t ctx;
    mem_zero(&ctx, sizeof(ctx));
    if (query_gop(&ctx.fb) != 0) {
        out(L"   FATAL: GOP not available\r\n");
        for(;;) __asm__("hlt");
    }
    print_dec(L"   width  = ", ctx.fb.width);
    print_dec(L"   height = ", ctx.fb.height);
    print_hex(L"   addr   = ", ctx.fb.addr, 16);
    ctx.initrd_start = initrd_phys;
    ctx.initrd_size  = isize;

    // --- Live-маркер --------------------------------------------------------
    // \live.flag присутствует только на установочном носителе (live-esp.img в ISO);
    // на установленном диске его нет.  Если он есть — просим ядро открыть
    // установщик, синтезируя cmdline "live installer".
    ctx.cmdline = 0;
    {
        void* fbuf = NULL;
        UINTN fsz  = 0;
        if (!EFI_ERROR(read_esp_file(L"\\live.flag", &fbuf, &fsz))) {
            static const char live_cmdline[] = "live installer";
            ctx.cmdline = live_cmdline;
            out(L"   live.flag present -> installer mode\r\n");
            if (fbuf) uefi_call_wrapper(g_bs->FreePool, 1, fbuf);
        }
    }

    // --- RSDP ---------------------------------------------------------------
    out(L"[5/6] Locating ACPI RSDP...\r\n");
    ctx.rsdp = find_rsdp(&ctx.rsdp_is_v2);
    if (ctx.rsdp) {
        ctx.rsdp_size = ctx.rsdp_is_v2 ? 36 : 20;
        print_hex(L"   RSDP @ ", (UINT64)(uintptr_t)ctx.rsdp, 16);
        if (ctx.rsdp_is_v2) out(L"   (ACPI 2.0+ XSDP)\r\n");
        else                out(L"   (ACPI 1.0 RSDP)\r\n");
    } else {
        out(L"   WARN: no RSDP — ACPI will be unavailable\r\n");
    }

    // --- Строим инфоблок (без mmap) -----------------------------------------
    UINTN info_used = 0;
    UINT8* info = build_mb2_info_partial(&ctx, &info_used);
    if (!info) {
        out(L"   FATAL: info pool alloc failed\r\n");
        for(;;) __asm__("hlt");
    }

    // --- Цикл GetMemoryMap → ExitBootServices -------------------------------
    out(L"[6/6] ExitBootServices...\r\n");

    EFI_MEMORY_DESCRIPTOR* memmap = NULL;
    UINTN  map_size = 0, map_key = 0, desc_size = 0;
    UINT32 desc_ver = 0;
    UINT64 total_ram = 0;

    // 1-й проход: узнаём требуемый размер.
    s = uefi_call_wrapper(g_bs->GetMemoryMap, 5,
        &map_size, memmap, &map_key, &desc_size, &desc_ver);
    if (s != EFI_BUFFER_TOO_SMALL) {
        out(L"   FATAL: GetMemoryMap probe failed\r\n");
        for(;;) __asm__("hlt");
    }
    // Резервируем место с запасом — у нас в info ещё много пустого буфера.
    map_size += desc_size * 16;
    memmap = (EFI_MEMORY_DESCRIPTOR*)uefi_pool_alloc(map_size);
    if (!memmap) {
        out(L"   FATAL: memmap alloc failed\r\n");
        for(;;) __asm__("hlt");
    }

    // Цикл попыток ExitBootServices.
    for (int attempt = 0; attempt < 5; attempt++) {
        UINTN sz = map_size;
        s = uefi_call_wrapper(g_bs->GetMemoryMap, 5,
            &sz, memmap, &map_key, &desc_size, &desc_ver);
        if (EFI_ERROR(s)) {
            map_size = sz + desc_size * 8;
            uefi_call_wrapper(g_bs->FreePool, 1, memmap);
            memmap = (EFI_MEMORY_DESCRIPTOR*)uefi_pool_alloc(map_size);
            if (!memmap) {
                out(L"   FATAL: memmap realloc failed\r\n");
                for(;;) __asm__("hlt");
            }
            continue;
        }

        // Пишем mmap-тэг в info-буфер (поверх старого, если был).
        UINTN mmap_off = info_used;
        UINTN mmap_len = write_tag_mmap(info + mmap_off, memmap, sz, desc_size,
                                        &total_ram);
        // Обновляем basic_meminfo (mem_upper) — находится в начале при off=8.
        UINT32* meminfo_words = (UINT32*)(info + 8);
        UINT64 upper_kb = total_ram / 1024;
        if (upper_kb > 0xFFFFFFFFu) upper_kb = 0xFFFFFFFFu;
        meminfo_words[3] = (UINT32)upper_kb;

        UINTN total_off = mmap_off + mmap_len;
        total_off += write_tag_end(info + total_off);
        total_off = (total_off + 7) & ~7u;

        // total_size + reserved
        ((UINT32*)info)[0] = (UINT32)total_off;
        ((UINT32*)info)[1] = 0;

        // Попытка ExitBootServices.
        s = uefi_call_wrapper(g_bs->ExitBootServices, 2, g_image, map_key);
        if (!EFI_ERROR(s)) break;       // success — мы вышли из EFI
        // Иначе пробуем снова (EFI мог поменять memmap между вызовами).
    }
    if (EFI_ERROR(s)) {
        out(L"   FATAL: ExitBootServices failed after 5 attempts\r\n");
        for(;;) __asm__("hlt");
    }

    // ВСЁ. С этого момента ConOut/AllocatePool недоступны — EFI services
    // выгружены. Теперь UART наш, можно его проинициализировать в режиме,
    // ожидаемом нашим kernel'ом (38400 baud, как у serial_init() в ядре —
    // на QEMU stdio это безразлично, но на реальном железе совпадение
    // оказалось бы важным).
    com1_init();
    com1_puts("[uefi] EBS done, jumping to kernel _efi_start\n");
    com1_puts("[uefi]   entry = ");
    com1_puthex(k_entry);
    com1_puts("\n[uefi]   info  = ");
    com1_puthex((UINT64)(uintptr_t)info);
    com1_puts("\n");

    // --- Прыгаем в kernel ---------------------------------------------------
    // _efi_start ждёт в RDI указатель на mb2_info_t. Сам выставит magic
    // и вызовет kernel_main64 c корректными аргументами.
    typedef __attribute__((sysv_abi)) void (*kentry_fn)(uint64_t info_ptr);
    kentry_fn entry = (kentry_fn)(uintptr_t)k_entry;
    entry((UINT64)(uintptr_t)info);

    // Не должно сюда дойти.
    for(;;) __asm__("hlt");
    return EFI_SUCCESS;
}

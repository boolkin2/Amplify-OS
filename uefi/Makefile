# ============================================================================
# uefi/Makefile — сборка BOOTX64.EFI (этап 38).
# ----------------------------------------------------------------------------
# Сборка собственно EFI-приложения. ESP-образ собирается родительским
# Makefile.x86_64 (он умеет подмешивать туда kernel.bin + initrd.img).
# Используется gnu-efi из /usr/lib (Debian/Ubuntu пакет gnu-efi).
# Собирает PIC объект, линкует как shared library, потом objcopy в EFI app.
# ============================================================================

EFI_INC      = /usr/include/efi
EFI_INC_X64  = $(EFI_INC)/x86_64
EFI_LIB      = /usr/lib
EFI_CRT      = $(EFI_LIB)/crt0-efi-x86_64.o
EFI_LDS      = $(EFI_LIB)/elf_x86_64_efi.lds

CC      = gcc
LD      = ld
OBJCOPY = objcopy

CFLAGS = -fno-stack-protector -fpic -fshort-wchar -mno-red-zone -Wall \
         -DEFI_FUNCTION_WRAPPER \
         -I$(EFI_INC) -I$(EFI_INC_X64) -I$(EFI_INC)/protocol

LDFLAGS = -nostdlib -znocombreloc -T $(EFI_LDS) -shared -Bsymbolic \
          -L $(EFI_LIB) $(EFI_CRT)

# Каталог сборки UEFI — внутри общего build/.
BUILD = ../build/uefi

EFI_TARGET = $(BUILD)/BOOTX64.EFI
SRC        = main.c
OBJ        = $(BUILD)/main.o
SO         = $(BUILD)/main.so

.PHONY: all clean

all: $(EFI_TARGET)

$(BUILD):
	@mkdir -p $(BUILD)

$(OBJ): $(SRC) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(SO): $(OBJ)
	$(LD) $(LDFLAGS) $< -o $@ -lefi -lgnuefi $(shell $(CC) -print-libgcc-file-name)

# Преобразование ELF shared object -> PE/COFF EFI application.
# КРИТИЧНО: -j .rodata — иначе все L"" CHAR16-строки (живут в .rodata благодаря
# -fshort-wchar) пропадают из .efi, и OutputString читает мусор по их адресам
# (мы это видели как поток символа U+BFAF на serial).  Старые версии gnu-efi
# мерджили .rodata в .data через elf_x86_64_efi.lds, новые — оставляют
# раздельно, поэтому секцию нужно явно перенести.
$(EFI_TARGET): $(SO)
	$(OBJCOPY) -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym \
	           -j .rel -j .rela -j .reloc -j .eh_frame \
	           --target=efi-app-x86_64 $< $@
	@echo "+ EFI image: $@"

clean:
	rm -f $(OBJ) $(SO) $(EFI_TARGET)
	rm -rf $(BUILD)

# ============================================================================
# kernel64/Makefile — сборка 64-битного ядра AMPLIFY-OS (Stage 33C).
# ============================================================================
# Производит build/kernel64.bin (Multiboot1 ELF64) и build/myos64.iso
# (загрузочный ISO для GRUB).
#
# Используется host-toolchain (gcc/ld умеют 64-битный ELF).
# ============================================================================

CC      = gcc
LD      = ld
NASM    = nasm

CFLAGS  = -m64 -ffreestanding -fno-stack-protector -fno-pie -mno-red-zone \
          -mcmodel=kernel -mno-mmx -mno-sse -mno-sse2 \
          -nostdlib -fno-builtin -Wall -Wextra -O2

LDFLAGS = -m elf_x86_64 -nostdlib -static -T linker.ld -z max-page-size=0x1000

ASMFLAGS = -felf64

BUILD = ../build/kernel64
ISO   = ../build/myos64.iso
ISODIR = ../build/iso64

KERNEL = ../build/kernel64.bin

OBJS = $(BUILD)/boot.o $(BUILD)/kmain.o

.PHONY: all iso clean

all: $(KERNEL) iso

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/boot.o: boot.asm | $(BUILD)
	$(NASM) $(ASMFLAGS) $< -o $@

$(BUILD)/kmain.o: kmain.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@echo "✓ 64-bit kernel: $@"

iso: $(ISO)

$(ISO): $(KERNEL)
	@rm -rf $(ISODIR)
	@mkdir -p $(ISODIR)/boot/grub
	@cp $(KERNEL) $(ISODIR)/boot/kernel64.bin
	@printf '%s\n' \
	    'set timeout=0' \
	    'set default=0' \
	    'menuentry "AMPLIFY-OS 64-bit (Stage 33C)" {' \
	    '    multiboot /boot/kernel64.bin' \
	    '    boot' \
	    '}' > $(ISODIR)/boot/grub/grub.cfg
	@grub-mkrescue -o $(ISO) $(ISODIR) 2>/dev/null
	@echo "✓ ISO: $(ISO)"

clean:
	rm -rf $(BUILD) $(KERNEL) $(ISO) $(ISODIR)

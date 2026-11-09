# ============================================================================
# Главный диспетчер AMPLIFY-OS Makefile.
# ----------------------------------------------------------------------------
# На этапе 37.6c (финал ФАЗЫ 37) DEFAULT стал ARCH=x86_64 — полный
# 64-битный путь со всеми подсистемами.  Старая 32-bit сборка остаётся
# доступна через `make ARCH=i386` для отладки или сравнения.
#
#   make                       — DEFAULT: 64-bit (этапы 37.1-37.6)
#   make ARCH=i386 [target]    — 32-bit (этапы 1-36)
#   make ARCH=x86_64 [target]  — явно 64-bit (то же что без ARCH=)
# При ARCH=x86_64 вся логика вынесена в Makefile.x86_64.
# ============================================================================
ARCH ?= x86_64
export ARCH

ifeq ($(ARCH),x86_64)
include Makefile.x86_64
else

# Makefile для MyOS (Этап 23 — Neural Desktop Builder)
# Компиляция и сборка операционной системы

# Инструменты сборки
ASM = nasm
CC = gcc
LD = ld
GRUB_MKRESCUE = grub-mkrescue
QEMU = qemu-system-i386
PYTHON ?= python3

# Флаги компиляции
ASMFLAGS = -f elf32
CFLAGS = -m32 -ffreestanding -O2 -Wall -Wextra -fno-exceptions -fno-pie -fno-stack-protector -mno-sse -mno-sse2 -mno-mmx -mfpmath=387 -fno-delete-null-pointer-checks -fno-strict-aliasing -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -finput-charset=UTF-8 -fexec-charset=CP1251
LDFLAGS = -m elf_i386 -T linker.ld -nostdlib

# Путь к libgcc (понадобится для 32/64-битного деления, float->int и т.д.,
# которые появляются из-за stb_truetype, stb_image)
LIBGCC := $(shell $(CC) -m32 -print-libgcc-file-name 2>/dev/null)

# Отдельные флаги для stb_impl.c — подавляем предупреждения сторонней библиотеки
STB_CFLAGS = -m32 -ffreestanding -O2 -w -fno-exceptions -fno-pie -fno-stack-protector -mno-sse -mno-sse2 -mno-mmx -mfpmath=387 -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0

# Флаги для userspace ELF программ
USER_CFLAGS = -m32 -ffreestanding -O2 -Wall -fno-exceptions -fno-pie -fno-stack-protector -nostdlib
USER_LDFLAGS = -m elf_i386 -T userspace/user_elf.ld -nostdlib

# Директории
BUILD_DIR = build
ISO_DIR = iso
BOOT_DIR = boot
KERNEL_DIR = kernel
TOOLS_DIR = tools
INITRD_DIR = initrd_content

# Целевые файлы
KERNEL_BIN = $(BUILD_DIR)/kernel.bin
ISO_FILE = $(BUILD_DIR)/myos.iso
INITRD_IMG = $(BUILD_DIR)/initrd.img
MKINITRD = $(BUILD_DIR)/mkinitrd
DISK_IMG = $(BUILD_DIR)/disk.img
USER_APP_BIN = $(INITRD_DIR)/app.bin

# ELF программы (Этап 18)
HELLO_ELF = $(INITRD_DIR)/hello.elf
ELFTEST_ELF = $(INITRD_DIR)/elftest.elf
FORKTEST_ELF = $(INITRD_DIR)/forktest.elf
EXECTEST_ELF = $(INITRD_DIR)/exectest.elf
CWDTEST_ELF  = $(INITRD_DIR)/cwdtest.elf
SHMTEST_ELF  = $(INITRD_DIR)/shmtest.elf

# Исходные файлы
ASM_SOURCES = kernel/arch/i386/boot.asm kernel/arch/i386/gdt_flush.asm kernel/arch/i386/idt_flush.asm kernel/arch/i386/isr.asm kernel/arch/i386/irq.asm kernel/arch/i386/user_mode.asm kernel/arch/i386/smp_trampoline.asm kernel/arch/i386/longmode_demo.asm
C_SOURCES = kernel/kernel.c kernel/libgcc_div64.c kernel/vga.c kernel/arch/i386/gdt.c kernel/arch/i386/idt.c kernel/arch/i386/interrupt.c kernel/arch/i386/syscall.c kernel/arch/i386/acpi.c kernel/arch/i386/lapic.c kernel/arch/i386/ioapic.c kernel/arch/i386/cpu_stats.c kernel/arch/i386/smp.c kernel/arch/i386/smp_work.c kernel/arch/i386/power.c kernel/arch/i386/longmode.c kernel/drivers/keyboard.c kernel/drivers/timer.c kernel/drivers/rtc.c kernel/drivers/mouse.c kernel/drivers/speaker.c kernel/drivers/ata.c kernel/drivers/pci.c kernel/drivers/ac97.c kernel/drivers/rtl8139.c kernel/drivers/usb/uhci.c kernel/drivers/usb/usb.c kernel/drivers/usb/usb_msd.c kernel/drivers/usb/scsi.c kernel/drivers/usb/usb_block.c kernel/sound/audio.c kernel/net/netif.c kernel/net/ethernet.c kernel/net/arp.c kernel/net/ipv4.c kernel/net/icmp.c kernel/net/udp.c kernel/net/dhcp.c kernel/net/tcp.c kernel/net/dns.c kernel/net/netcfg.c kernel/net/http.c kernel/net/ping.c kernel/memory/memory.c kernel/memory/pmm.c kernel/memory/heap.c kernel/memory/paging.c kernel/memory/vm.c kernel/memory/swap.c kernel/ipc/shm.c kernel/ipc/pipe.c kernel/ipc/unix_socket.c kernel/ipc/signal.c kernel/fs/vfs.c kernel/fs/initrd.c kernel/fs/fat32.c kernel/fs/disk_io.c kernel/security/security.c kernel/security/users.c kernel/security/perms.c kernel/security/sec_ui.c kernel/security/firstboot.c kernel/security/login.c kernel/shell/shell.c kernel/shell/sh_io.c kernel/shell/pipecmds.c kernel/task/task.c kernel/gui/framebuffer.c kernel/gui/graphics.c kernel/gui/font.c kernel/gui/doublebuf.c kernel/gui/cursor.c kernel/gui/window.c kernel/gui/wm.c kernel/gui/desktop.c kernel/gui/apps.c kernel/gui/startmenu.c kernel/gui/ttf.c kernel/gui/image.c kernel/loader/elf.c kernel/loader/dynlink.c

# Исходники stb (компилируются отдельно с ослабленными предупреждениями)
STB_SOURCES = kernel/gui/stb_impl.c
STB_OBJECTS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(STB_SOURCES))

# Файлы для InitRD
INITRD_FILES = $(wildcard $(INITRD_DIR)/*)

# Объектные файлы
ASM_OBJECTS = $(patsubst %.asm,$(BUILD_DIR)/%.o,$(ASM_SOURCES))
C_OBJECTS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(C_SOURCES))
OBJECTS = $(ASM_OBJECTS) $(C_OBJECTS) $(STB_OBJECTS)

# Цвета для вывода
GREEN = \033[0;32m
YELLOW = \033[0;33m
RED = \033[0;31m
CYAN = \033[0;36m
NC = \033[0m # No Color

.PHONY: all clean run debug dirs initrd disk userspace assets ui-layout ui-clean uefi run-uefi kernel64 run-64 run-tap setup-tap teardown-tap status-tap

# Главная цель
all: dirs ui-layout $(ISO_FILE) disk usbstick
	@echo "$(GREEN)✓ Сборка завершена успешно!$(NC)"
	@echo "$(YELLOW)Запусти 'make run' для запуска в QEMU$(NC)"

# ============================================================================
# Этап 23 — Neural Desktop Builder
# Запускает Python-скрипт, который читает ui_mockups/ и генерирует
# kernel/gui/ui_theme.h и kernel/gui/ui_layout.inc.
# Скрипт УСТОЙЧИВ: если макетов/API/Pillow нет — запишет safe default
# и сборка пройдёт без AI. Никогда не ломает билд.
# ============================================================================
UI_THEME_H   = kernel/gui/ui_theme.h
UI_LAYOUT_INC = kernel/gui/ui_layout.inc
AI_UI_SCRIPT = tools/ai_ui_gen.py
UI_MOCKUP_FILES := $(wildcard ui_mockups/*.png) $(wildcard ui_mockups/*.jpg) \
                   $(wildcard ui_mockups/*.jpeg) $(wildcard ui_mockups/*.bmp) \
                   $(wildcard ui_mockups/*.webp)

# Стемпфайл гарантирует, что один запуск скрипта создаёт оба файла,
# а не вызывает его дважды. Работает на любом GNU Make >= 3.81.
UI_STAMP = $(BUILD_DIR)/.ui_layout.stamp

$(UI_STAMP): $(AI_UI_SCRIPT) $(UI_MOCKUP_FILES) | dirs
	@echo "$(CYAN)🧠 Neural Desktop Builder: анализ ui_mockups/ ($(words $(UI_MOCKUP_FILES)) макетов)...$(NC)"
	@$(PYTHON) $(AI_UI_SCRIPT) || (echo "$(RED)ai_ui_gen.py упал — оставляю предыдущие ui_theme.h/ui_layout.inc$(NC)"; true)
	@touch $(UI_STAMP)
	@echo "$(GREEN)✓ UI layout сгенерирован$(NC)"

# Сами файлы помечаем зависимыми от стемпа — если стемпа нет, скрипт запустится.
$(UI_THEME_H) $(UI_LAYOUT_INC): $(UI_STAMP)

ui-layout: $(UI_STAMP)

ui-clean:
	@rm -f $(UI_THEME_H) $(UI_LAYOUT_INC) $(UI_STAMP)

# apps.c и wm.c явно зависят от сгенерированных файлов.
$(BUILD_DIR)/kernel/gui/apps.o: $(UI_THEME_H) $(UI_LAYOUT_INC)
$(BUILD_DIR)/kernel/gui/wm.o:   $(UI_THEME_H)

# Создание директорий
dirs:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BUILD_DIR)/kernel/arch/i386
	@mkdir -p $(BUILD_DIR)/kernel/drivers
	@mkdir -p $(BUILD_DIR)/kernel/drivers/usb
	@mkdir -p $(BUILD_DIR)/kernel/memory
	@mkdir -p $(BUILD_DIR)/kernel/fs
	@mkdir -p $(BUILD_DIR)/kernel/security
	@mkdir -p $(BUILD_DIR)/kernel/shell
	@mkdir -p $(BUILD_DIR)/kernel/task
	@mkdir -p $(BUILD_DIR)/kernel/gui
	@mkdir -p $(BUILD_DIR)/kernel/loader
	@mkdir -p $(BUILD_DIR)/kernel/ipc
	@mkdir -p $(BUILD_DIR)/kernel/sound
	@mkdir -p $(BUILD_DIR)/kernel/net
	@mkdir -p $(BUILD_DIR)/userspace/hello
	@mkdir -p $(BUILD_DIR)/userspace/elftest
	@mkdir -p $(BUILD_DIR)/userspace/forktest
	@mkdir -p $(BUILD_DIR)/userspace/exectest
	@mkdir -p $(BUILD_DIR)/userspace/cwdtest
	@mkdir -p $(BUILD_DIR)/userspace/shmtest
	@mkdir -p $(BUILD_DIR)/userspace/libmyos
	@mkdir -p $(ISO_DIR)/boot/grub

# Сборка утилиты mkinitrd (HOST-компиляция, обычный gcc)
$(MKINITRD): $(TOOLS_DIR)/mkinitrd.c
	@echo "$(YELLOW)Компиляция mkinitrd (host tool)...$(NC)"
	@gcc -o $@ $< -Wall -Wextra
	@echo "$(GREEN)✓ mkinitrd собран$(NC)"

# Сборка пользовательского приложения (старый формат — flat binary)
$(USER_APP_BIN): userspace/app.c userspace/user_linker.ld
	@echo "$(YELLOW)Компиляция пользовательского приложения...$(NC)"
	@$(CC) $(CFLAGS) -c $< -o $(BUILD_DIR)/app.o
	@$(LD) -m elf_i386 -T userspace/user_linker.ld -o $@ $(BUILD_DIR)/app.o --oformat binary
	@echo "$(GREEN)✓ Приложение собрано: $@$(NC)"

# ============================================================================
# Сборка ELF userspace программ (Этап 18 и 19)
# ============================================================================

# Флаги для libc (Этап 19 и 20)
LIBC_CFLAGS = $(USER_CFLAGS) -Ilibc/include
LIBC_SOURCES = libc/src/stdio.c libc/src/stdlib.c libc/src/string.c libc/src/unistd.c libc/src/pthread.c libc/src/sys_mman.c
LIBC_OBJECTS = $(patsubst libc/src/%.c,$(BUILD_DIR)/libc/%.o,$(LIBC_SOURCES))
LIBC_A = $(BUILD_DIR)/libc/libc.a

# Правило компиляции исходников libc
$(BUILD_DIR)/libc/%.o: libc/src/%.c
	@echo "$(YELLOW)Компиляция libc: $<...$(NC)"
	@mkdir -p $(BUILD_DIR)/libc
	@$(CC) $(LIBC_CFLAGS) -c $< -o $@

# Сборка статической библиотеки libc.a
$(LIBC_A): $(LIBC_OBJECTS)
	@echo "$(YELLOW)Сборка libc.a...$(NC)"
	@ar rcs $@ $^
	@echo "$(GREEN)✓ libc.a собрана$(NC)"

# Библиотека CRT (start.o — точка входа _start)
$(BUILD_DIR)/userspace/libmyos/start.o: userspace/libmyos/start.c
	@echo "$(YELLOW)Компиляция CRT0 (start.c)...$(NC)"
	@$(CC) $(USER_CFLAGS) -c $< -o $@

# Hello World ELF (с поддержкой libc)
$(HELLO_ELF): userspace/hello/main.c $(BUILD_DIR)/userspace/libmyos/start.o userspace/user_elf.ld $(LIBC_A)
	@echo "$(YELLOW)Компиляция hello.elf (с libc)...$(NC)"
	@$(CC) $(LIBC_CFLAGS) -c $< -o $(BUILD_DIR)/userspace/hello/main.o
	@$(LD) $(USER_LDFLAGS) -o $@ $(BUILD_DIR)/userspace/libmyos/start.o $(BUILD_DIR)/userspace/hello/main.o $(LIBC_A)
	@echo "$(GREEN)✓ hello.elf собран$(NC)"

# ELF Test (GUI gradient)
$(ELFTEST_ELF): userspace/elftest/main.c $(BUILD_DIR)/userspace/libmyos/start.o userspace/user_elf.ld
	@echo "$(YELLOW)Компиляция elftest.elf...$(NC)"
	@$(CC) $(USER_CFLAGS) -c $< -o $(BUILD_DIR)/userspace/elftest/main.o
	@$(LD) $(USER_LDFLAGS) -o $@ $(BUILD_DIR)/userspace/libmyos/start.o $(BUILD_DIR)/userspace/elftest/main.o
	@echo "$(GREEN)✓ elftest.elf собран$(NC)"

# Fork Test (POSIX fork + COW)
$(FORKTEST_ELF): userspace/forktest/main.c $(BUILD_DIR)/userspace/libmyos/start.o userspace/user_elf.ld
	@echo "$(YELLOW)Компиляция forktest.elf...$(NC)"
	@$(CC) $(USER_CFLAGS) -c $< -o $(BUILD_DIR)/userspace/forktest/main.o
	@$(LD) $(USER_LDFLAGS) -o $@ $(BUILD_DIR)/userspace/libmyos/start.o $(BUILD_DIR)/userspace/forktest/main.o
	@echo "$(GREEN)✓ forktest.elf собран$(NC)"

# Exec Test (POSIX execve)
$(EXECTEST_ELF): userspace/exectest/main.c $(BUILD_DIR)/userspace/libmyos/start.o userspace/user_elf.ld
	@echo "$(YELLOW)Компиляция exectest.elf...$(NC)"
	@$(CC) $(USER_CFLAGS) -c $< -o $(BUILD_DIR)/userspace/exectest/main.o
	@$(LD) $(USER_LDFLAGS) -o $@ $(BUILD_DIR)/userspace/libmyos/start.o $(BUILD_DIR)/userspace/exectest/main.o
	@echo "$(GREEN)✓ exectest.elf собран$(NC)"

# Cwd Test (chdir/getcwd + '..')
$(CWDTEST_ELF): userspace/cwdtest/main.c $(BUILD_DIR)/userspace/libmyos/start.o userspace/user_elf.ld
	@echo "$(YELLOW)Компиляция cwdtest.elf...$(NC)"
	@$(CC) $(USER_CFLAGS) -c $< -o $(BUILD_DIR)/userspace/cwdtest/main.o
	@$(LD) $(USER_LDFLAGS) -o $@ $(BUILD_DIR)/userspace/libmyos/start.o $(BUILD_DIR)/userspace/cwdtest/main.o
	@echo "$(GREEN)✓ cwdtest.elf собран$(NC)"

# Shm Test (POSIX shared memory + fork)
$(SHMTEST_ELF): userspace/shmtest/main.c $(BUILD_DIR)/userspace/libmyos/start.o userspace/user_elf.ld
	@echo "$(YELLOW)Компиляция shmtest.elf...$(NC)"
	@$(CC) $(USER_CFLAGS) -c $< -o $(BUILD_DIR)/userspace/shmtest/main.o
	@$(LD) $(USER_LDFLAGS) -o $@ $(BUILD_DIR)/userspace/libmyos/start.o $(BUILD_DIR)/userspace/shmtest/main.o
	@echo "$(GREEN)✓ shmtest.elf собран$(NC)"

# Цель для сборки всех ELF программ
userspace: dirs $(HELLO_ELF) $(ELFTEST_ELF) $(FORKTEST_ELF) $(EXECTEST_ELF) $(CWDTEST_ELF) $(SHMTEST_ELF)
	@echo "$(GREEN)✓ Все ELF программы собраны$(NC)"

# Ресурсы Этапа 21 — TTF-шрифт и PNG-баннер
DEFAULT_TTF = $(INITRD_DIR)/default.ttf
BANNER_PNG  = $(INITRD_DIR)/banner.png
MAKEBANNER  = $(BUILD_DIR)/makebanner

# Ресурсы Этапа 24 — звук запуска
STARTUP_WAV   = $(INITRD_DIR)/startup.wav
MAKESTARTUP   = $(BUILD_DIR)/makestartup

$(MAKESTARTUP): $(TOOLS_DIR)/makestartup.c
	@echo "$(YELLOW)Компиляция makestartup (host tool)...$(NC)"
	@mkdir -p $(BUILD_DIR)
	@gcc -O2 -Wall -o $@ $< -lm
	@echo "$(GREEN)✓ makestartup собран$(NC)"

$(STARTUP_WAV): $(MAKESTARTUP)
	@echo "$(YELLOW)Генерация startup.wav...$(NC)"
	@$(MAKESTARTUP) $@
	@echo "$(GREEN)✓ startup.wav создан$(NC)"

# Host-утилита для генерации PNG (требует libz-dev)
$(MAKEBANNER): $(TOOLS_DIR)/makebanner.c
	@echo "$(YELLOW)Компиляция makebanner (host tool)...$(NC)"
	@mkdir -p $(BUILD_DIR)
	@gcc -O2 -Wall -o $@ $< -lz
	@echo "$(GREEN)✓ makebanner собран$(NC)"

$(DEFAULT_TTF):
	@echo "$(YELLOW)Поиск/загрузка TTF-шрифта...$(NC)"
	@bash scripts/fetch_font.sh $@ || true

$(BANNER_PNG): $(MAKEBANNER)
	@echo "$(YELLOW)Генерация PNG-баннера...$(NC)"
	@$(MAKEBANNER) $@

assets: $(DEFAULT_TTF) $(BANNER_PNG)
	@echo "$(GREEN)✓ Ассеты (шрифт + баннер) готовы$(NC)"

# Создание образа InitRD (зависит от ELF программ + ассеты Этапа 21)
$(INITRD_IMG): $(MKINITRD) $(USER_APP_BIN) $(HELLO_ELF) $(ELFTEST_ELF) $(FORKTEST_ELF) $(EXECTEST_ELF) $(CWDTEST_ELF) $(SHMTEST_ELF) $(DEFAULT_TTF) $(BANNER_PNG) $(STARTUP_WAV) $(wildcard $(INITRD_DIR)/*)
	@echo "$(YELLOW)Создание образа InitRD...$(NC)"
	@$(MKINITRD) $(INITRD_IMG) $(INITRD_DIR)/*
	@echo "$(GREEN)✓ InitRD образ создан: $(INITRD_IMG)$(NC)"

# Сборка ISO образа (теперь зависит от InitRD)
$(ISO_FILE): $(KERNEL_BIN) $(INITRD_IMG)
	@echo "$(YELLOW)Создание ISO образа...$(NC)"
	@cp $(KERNEL_BIN) $(ISO_DIR)/boot/kernel.bin
	@cp $(INITRD_IMG) $(ISO_DIR)/boot/initrd.img
	@cp boot/grub.cfg $(ISO_DIR)/boot/grub/grub.cfg 2>/dev/null || echo "menu entry MyOS" > $(ISO_DIR)/boot/grub/grub.cfg
	@if command -v $(GRUB_MKRESCUE) >/dev/null 2>&1; then \
		$(GRUB_MKRESCUE) -o $(ISO_FILE) $(ISO_DIR) >/dev/null 2>&1 \
			&& echo "$(GREEN)✓ ISO образ создан: $(ISO_FILE)$(NC)" \
			|| echo "$(YELLOW)⚠ ISO не собран (grub-mkrescue упал — ядро запускается напрямую через -kernel)$(NC)"; \
	else \
		echo "$(YELLOW)⚠ grub-mkrescue не найден — пропускаю ISO (используй 'make run-kernel')$(NC)"; \
	fi

# Создание виртуального диска (FAT32)
disk: dirs $(DISK_IMG)

$(DISK_IMG):
	@echo "$(YELLOW)Создание виртуального диска FAT32 (64MB)...$(NC)"
	@bash scripts/mkdisk.sh $@
	@echo "$(GREEN)✓ Диск FAT32 готов: $@$(NC)"

# ============================================================================
# Этап 28 — USB Mass Storage
# Виртуальная флешка: 32 MB, FAT32. QEMU подключит её через UHCI как usb-storage.
# При первом запуске на ней будет один файл README.TXT, чтобы шелл/проводник
# сразу могли что-то увидеть в /usb.
# ============================================================================
USB_IMG = $(BUILD_DIR)/usb.img

usbstick: dirs $(USB_IMG)

$(USB_IMG):
	@echo "$(YELLOW)Создание виртуальной USB-флешки FAT32 (32MB)...$(NC)"
	@dd if=/dev/zero of=$@ bs=1M count=32 2>/dev/null
	@mkfs.vfat -F 32 $@ >/dev/null 2>&1 || mformat -i $@ -F :: 2>/dev/null || true
	@if command -v mcopy >/dev/null 2>&1; then \
		TMPF=$$(mktemp /tmp/myos_usb_hello.XXXXXX); \
		echo "Privet iz USB!" > $$TMPF; \
		mcopy -i $@ $$TMPF ::/HELLO.TXT >/dev/null 2>&1 || true; \
		rm -f $$TMPF; \
	fi
	@echo "$(GREEN)✓ USB-образ готов: $@$(NC)"

# Линковка ядра (добавляем libgcc для math helper-ов, которые создаёт stb)
$(KERNEL_BIN): $(OBJECTS)
	@echo "$(YELLOW)Линковка ядра...$(NC)"
	@$(LD) $(LDFLAGS) -o $@ $^ $(LIBGCC)
	@echo "$(GREEN)✓ Ядро скомпилировано: $(KERNEL_BIN)$(NC)"

# Отдельное правило для stb_impl.c (без -Wall -Wextra)
$(BUILD_DIR)/kernel/gui/stb_impl.o: kernel/gui/stb_impl.c kernel/gui/stb_config.h kernel/gui/stb_truetype.h kernel/gui/stb_image.h
	@echo "$(YELLOW)Компиляция $< (STB, без предупреждений)...$(NC)"
	@mkdir -p $(dir $@)
	@$(CC) $(STB_CFLAGS) -c $< -o $@

# Компиляция ассемблерных файлов
$(BUILD_DIR)/%.o: %.asm
	@echo "$(YELLOW)Компиляция $<...$(NC)"
	@mkdir -p $(dir $@)
	@$(ASM) $(ASMFLAGS) $< -o $@

# Компиляция C файлов
$(BUILD_DIR)/%.o: %.c
	@echo "$(YELLOW)Компиляция $<...$(NC)"
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

# ----------------------------------------------------------------------------
# Автоопределение аудио-бэкенда QEMU.
# На WSL2 + Windows 11 работает WSLg, он пробрасывает PulseAudio через
# /mnt/wslg/PulseServer. Если он доступен — используем pa. Иначе (WSL1,
# без WSLg, headless) — none: драйвер инициализируется, но звука не слышно.
# Для нативного Linux с PulseAudio (pactl info отрабатывает) — тоже pa.
# Можно принудительно задать: make run AUDIO=pa | AUDIO=sdl | AUDIO=none
# ----------------------------------------------------------------------------
AUDIO ?= $(shell if command -v pactl >/dev/null 2>&1 && pactl info >/dev/null 2>&1; \
                  then echo pa; else echo none; fi)

# Запуск в QEMU (VGA + PC Speaker + HDD + SMP + AC97 audio + RTL8139 net)
# Сеть:
#   -netdev user,id=net0    — slirp backend (NAT на хост, без root-прав)
#   -device rtl8139,...     — модель сетевой карты
#   -object filter-dump,... — пишет весь трафик eth0 в pcap,
#                             открываемый в Wireshark: wireshark build/eth0.pcap
#
# ВАЖНО: грузимся через ISO с GRUB (`-cdrom ... -boot d`), а НЕ через `-kernel`.
# Встроенный в QEMU multiboot-loader не настраивает VESA framebuffer (он сам
# говорит «multiboot knows VBE. we don't»), и без этого ядро уходит в текстовый
# shell-fallback, а рабочий стол не появляется. GRUB же читает из grub.cfg
# `set gfxpayload=1024x768x32` и передаёт ядру корректную framebuffer info.
run: all
	@echo "$(GREEN)╔══════════════════════════════════════╗$(NC)"
	@echo "$(GREEN)║     Запуск MyOS v2.0 в QEMU...       ║$(NC)"
	@echo "$(GREEN)╚══════════════════════════════════════╝$(NC)"
	@echo "$(YELLOW)Аудио-бэкенд QEMU: $(AUDIO)$(NC)"
	@$(QEMU) -cdrom $(ISO_FILE) -boot d -m 256M -vga std -smp 4 \
		-drive file=$(DISK_IMG),format=raw,index=0,media=disk \
		-audiodev $(AUDIO),id=audio0 \
		-machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-netdev user,id=net0 \
		-device rtl8139,netdev=net0 \
		-object filter-dump,id=f1,netdev=net0,file=$(BUILD_DIR)/eth0.pcap \
		-drive if=none,id=usbstick,format=raw,file=$(USB_IMG) \
		-device piix3-usb-uhci,id=uhci \
		-device usb-storage,bus=uhci.0,drive=usbstick

# ============================================================================
# Этап 34: запуск через TAP/bridge (реальный выход в интернет)
# ============================================================================
# Использует bridge backend QEMU + qemu-bridge-helper. Перед первым запуском
# нужно один раз настроить мост на хосте:
#
#     sudo bash tools/setup_tap.sh setup bridge   # проводной Ethernet
#     sudo bash tools/setup_tap.sh setup nat      # Wi-Fi / NAT-режим
#
# После этого `make run-tap` запускается БЕЗ sudo: QEMU через
# qemu-bridge-helper подключает свою TAP-карту к мосту br0, гость получает
# IP по DHCP (от роутера в bridge-режиме или от dnsmasq в nat-режиме) и
# выходит в интернет без хардкода.
#
# Ничего из user-mode (slirp) не трогаем — `make run` работает по-прежнему.
BRIDGE ?= br0

setup-tap:
	@sudo bash tools/setup_tap.sh setup $(MODE) $(IFACE)

teardown-tap:
	@sudo bash tools/setup_tap.sh teardown

status-tap:
	@bash tools/setup_tap.sh status

run-tap: all
	@echo "$(GREEN)╔══════════════════════════════════════════╗$(NC)"
	@echo "$(GREEN)║  Запуск AMPLIFY-OS (TAP/bridge: $(BRIDGE)) ║$(NC)"
	@echo "$(GREEN)╚══════════════════════════════════════════╝$(NC)"
	@if [ ! -f /run/amplify-tap.state ]; then \
		echo "$(YELLOW)⚠ TAP/bridge не настроен. Сначала запусти:$(NC)"; \
		echo "    sudo bash tools/setup_tap.sh setup bridge   # для проводного LAN"; \
		echo "    sudo bash tools/setup_tap.sh setup nat      # для Wi-Fi"; \
		exit 1; \
	fi
	@echo "$(YELLOW)Аудио-бэкенд QEMU: $(AUDIO)$(NC)"
	@$(QEMU) -cdrom $(ISO_FILE) -boot d -m 256M -vga std -smp 4 \
		-drive file=$(DISK_IMG),format=raw,index=0,media=disk \
		-audiodev $(AUDIO),id=audio0 \
		-machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-netdev bridge,id=net0,br=$(BRIDGE) \
		-device rtl8139,netdev=net0,mac=52:54:00:12:34:56 \
		-object filter-dump,id=f1,netdev=net0,file=$(BUILD_DIR)/eth0-tap.pcap \
		-drive if=none,id=usbstick,format=raw,file=$(USB_IMG) \
		-device piix3-usb-uhci,id=uhci \
		-device usb-storage,bus=uhci.0,drive=usbstick \
		-serial file:$(BUILD_DIR)/serial-tap.log

# ============================================================================
# Этап 33B: UEFI loader (gnu-efi)
# ============================================================================
# Собирает отдельный BOOTX64.EFI и кладёт его в FAT32-образ build/esp.img.
# Запуск: `make run-uefi` (через QEMU x86_64 + OVMF firmware).
# Это НЕ заменяет основной BIOS/GRUB-загрузочный путь — это второй,
# параллельный вариант загрузки (proof-of-concept перед этапом 33C).
uefi:
	@echo "$(GREEN)Сборка UEFI-загрузчика...$(NC)"
	@$(MAKE) -C uefi
	@echo "$(GREEN)✓ UEFI: build/uefi/BOOTX64.EFI и build/esp.img готовы$(NC)"

run-uefi: uefi
	@echo "$(GREEN)╔══════════════════════════════════════════╗$(NC)"
	@echo "$(GREEN)║   Запуск AMPLIFY-OS UEFI loader (OVMF)   ║$(NC)"
	@echo "$(GREEN)╚══════════════════════════════════════════╝$(NC)"
	@if [ -f /usr/share/ovmf/OVMF.fd ]; then \
		OVMF=/usr/share/ovmf/OVMF.fd ; \
	else \
		OVMF=/usr/share/OVMF/OVMF.fd ; \
	fi ; \
	qemu-system-x86_64 -bios $$OVMF \
		-drive format=raw,file=$(BUILD_DIR)/esp.img \
		-m 256 -net none

# ============================================================================
# Этап 33C: 64-битный кернел AMPLIFY-OS (kernel64.bin / myos64.iso)
# ============================================================================
# Параллельный 64-битный кернел: GRUB → Multiboot1 → 32-bit boot.asm →
# PML4 → Long Mode → kmain64. Не заменяет 32-битное ядро, а соседствует с ним.
# Запуск: `make run-64`.
kernel64:
	@echo "$(GREEN)Сборка 64-битного ядра AMPLIFY-OS...$(NC)"
	@$(MAKE) -C kernel64
	@echo "$(GREEN)✓ build/kernel64.bin и build/myos64.iso готовы$(NC)"

run-64: kernel64
	@echo "$(GREEN)╔══════════════════════════════════════════╗$(NC)"
	@echo "$(GREEN)║   Запуск AMPLIFY-OS 64-bit (Stage 33C)    ║$(NC)"
	@echo "$(GREEN)╚══════════════════════════════════════════╝$(NC)"
	@qemu-system-x86_64 -cdrom $(BUILD_DIR)/myos64.iso -boot d -m 256 \
		-serial stdio

# Старый путь через `-kernel` — без VESA framebuffer, останавливается в shell.
# Полезен для быстрой отладки текстового режима.
run-kernel: all
	@echo "$(YELLOW)Запуск MyOS через -kernel (без графики, только shell)...$(NC)"
	@$(QEMU) -kernel $(KERNEL_BIN) -m 256M -vga std -smp 4 \
		-drive file=$(DISK_IMG),format=raw,index=0,media=disk \
		-audiodev $(AUDIO),id=audio0 \
		-machine pcspk-audiodev=audio0 \
		-device AC97,audiodev=audio0 \
		-netdev user,id=net0 \
		-device rtl8139,netdev=net0 \
		-object filter-dump,id=f1,netdev=net0,file=$(BUILD_DIR)/eth0.pcap \
		-drive if=none,id=usbstick,format=raw,file=$(USB_IMG) \
		-device piix3-usb-uhci,id=uhci \
		-device usb-storage,bus=uhci.0,drive=usbstick

# Запуск без звука (если SDL аудио недоступен) — с serial-отладкой на stdout хоста
run-nosound: all
	@echo "$(GREEN)Запуск MyOS в QEMU (без звука, с serial debug)...$(NC)"
	@$(QEMU) -kernel $(KERNEL_BIN) -m 256M -vga std -smp 4 \
		-drive file=$(DISK_IMG),format=raw,index=0,media=disk -serial stdio \
		-drive if=none,id=usbstick,format=raw,file=$(USB_IMG) \
		-device piix3-usb-uhci,id=uhci \
		-device usb-storage,bus=uhci.0,drive=usbstick

# Серверная отладка (только serial, без графики) — для обнаружения ранних падений.
# Грузимся из ISO, чтобы получить framebuffer и пройти полный путь до WM.
run-debug: all
	@echo "$(GREEN)Запуск MyOS в QEMU с serial-отладкой в stdout...$(NC)"
	@$(QEMU) -cdrom $(ISO_FILE) -boot d -m 256M -vga std -smp 4 \
		-drive file=$(DISK_IMG),format=raw,index=0,media=disk -serial stdio -no-reboot -no-shutdown \
		-audiodev none,id=audio0 -device AC97,audiodev=audio0 \
		-netdev user,id=net0 \
		-device rtl8139,netdev=net0 \
		-object filter-dump,id=f1,netdev=net0,file=$(BUILD_DIR)/eth0.pcap

# Запуск в режиме отладки
debug: all
	@echo "$(GREEN)Запуск MyOS в режиме отладки...$(NC)"
	@$(QEMU) -kernel $(KERNEL_BIN) -m 256M -vga std -smp 4 -drive file=$(DISK_IMG),format=raw,index=0,media=disk -s -S &
	@echo "$(YELLOW)QEMU ожидает подключения GDB на порту 1234$(NC)"
	@echo "$(YELLOW)Запусти: gdb $(KERNEL_BIN) -ex 'target remote localhost:1234'$(NC)"

# Только InitRD (для тестирования)
initrd: dirs $(INITRD_IMG)
	@echo "$(GREEN)✓ InitRD готов$(NC)"

# Очистка
# Очистка
clean:
	@echo "$(YELLOW)Очистка...$(NC)"
	@rm -rf $(BUILD_DIR) 2>/dev/null || true
	@rm -rf $(ISO_DIR) 2>/dev/null || true
	@find $(BUILD_DIR) -type f -exec rm -f {} \; 2>/dev/null || true
	@echo "$(GREEN)✓ Очищено$(NC)"

endif    # ifeq ($(ARCH),x86_64)

#!/bin/bash
# ============================================================================
# test_stage38_1_reloc.sh — подэтап 38.1.1: ядро в верхней половине адресов.
# ----------------------------------------------------------------------------
# Ядро слинковано на KERNEL_VMA (0xFFFFFFFF80000000), а лежит в физической
# памяти там, куда его положил загрузчик. Проверяем:
#   0. раскладку образа (заголовки ELF, символы) и что предел конца образа в
#      Makefile умеет ронять сборку;
#   1. UEFI: загрузку с РАЗНЫХ физических адресов — место по умолчанию (верх
#      RAM ниже 4 ГБ) и три заданных файлом \kload.at (1 МБ, 161 МБ, 1 ГБ):
#      все ядра, диск и сеть на virtio (буферы DMA, часть — в образе ядра),
#      smoke-блоки MIND / FDE / VT-x / JIT; то же на 1 ядре и на q35 (AHCI);
#   2. отказы загрузчика: занятое место и сдвиг, не кратный 2 МБ;
#   3. образ, заведомо больший прежней «стены» 8 МБ (.bss + 8 МБ): грузится
#      под UEFI; положенный по старому адресу — внятный отказ загрузчика;
#   4. ветку BIOS (GRUB, Multiboot2, образ по LMA 1 МБ) — обычное ядро и
#      раздутое, с картой памяти до 16 МБ в журнале;
#   5. «тест умеет падать»: ядро с испорченным virt_to_phys (без сдвига)
#      проходит при сдвиге 0 и обязано провалиться при любом другом;
#   6. путь отката: ядро старой раскладки (make HIGHER_HALF=0) тем же
#      загрузчиком.
# Дополнительные ядра (раздутое, мутант, старое) собираются в свои каталоги
# (BUILD_DIR=build/x86_64-…), основная сборка не трогается.
#
#   AOS_38_SKIP_BUILDS=1 — без фаз 3–6 (только то, что не требует пересборки).
# Итог: «== stage 38.1.1 reloc: N passed, M failed, K skipped ==».
# ============================================================================
set -u
cd "$(dirname "$0")/.."

ESP=build/esp.img
DEVDISK=build/disk-x86_64.img
KERNEL=build/kernel-x86_64.bin
INITRD=build/initrd-x86_64.img
OUT_DIR=build/screens-38_1
WORK=/tmp/amplify-stage38_1
MK="make -f Makefile.x86_64"

PASS=0; FAIL=0; SKIP=0
ok()   { echo "  [PASS] $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }
skip() { echo "  [SKIP] $1"; SKIP=$((SKIP+1)); }
check(){ if eval "$2"; then ok "$1"; else bad "$1"; fi; }

OVMF=""
for f in /usr/share/pve-edk2-firmware/OVMF_CODE.fd /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF.fd /usr/share/edk2-ovmf/x64/OVMF.fd; do
    [ -f "$f" ] && { OVMF=$f; break; }
done
[ -n "$OVMF" ] || { echo "OVMF.fd not found"; exit 1; }
[ -f "$INITRD" ] || INITRD=$(ls build/initrd*.img 2>/dev/null | head -1)
for f in "$ESP" "$DEVDISK" "$KERNEL" "$INITRD"; do [ -f "$f" ] || { echo "$f not found — run: make ARCH=x86_64"; exit 1; }; done

if pgrep -f "[q]emu-system.*$WORK/" >/dev/null 2>&1; then
    echo "another run of this test is still alive (QEMU with $WORK) — wait for it or kill it"
    exit 1
fi
rm -rf "$WORK"; mkdir -p "$WORK" "$OUT_DIR"

QEMU=$(command -v qemu-system-x86_64 || echo /usr/bin/qemu-system-x86_64)
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then KVM=1; ACC="-enable-kvm -cpu host"; SLOW=1
else KVM=0; ACC="-accel tcg,thread=multi -cpu max"; SLOW=${AOS_SLOW:-8}; fi
echo "accel: $([ $KVM = 1 ] && echo KVM || echo "TCG x$SLOW")  OVMF: $OVMF"

READY='smoke 37.6c-1\] RTC'           # последняя строка загрузки перед экраном входа

# ---------------------------------------------------------------------------
# boot <имя> <прошивка: uefi|bios> <образ: esp либо iso> <ядер> <машина> <ждать, с> <что считать концом>
# Журнал — $WORK/<имя>.log. Диски с -snapshot: рабочие образы не меняются,
# а QEMU можно гасить kill -9 (CLAUDE.md §5.33).
# ---------------------------------------------------------------------------
boot() {
    local name=$1 fw=$2 img=$3 cpus=$4 mach=$5 secs=$(( $6 * SLOW )) stop=$7
    local log="$WORK/$name.log" args=()
    : > "$log"
    if [ "$fw" = uefi ]; then
        # ESP — тоже на virtio и ВТОРЫМ: том FAT ядро монтирует с первого
        # найденного диска, им должен быть системный (CLAUDE.md §8).
        args+=(-bios "$OVMF" -drive "format=raw,file=$img,if=none,id=esp,snapshot=on" -vga virtio)
    else
        args+=(-cdrom "$img" -boot d -vga std)
    fi
    if [ "$mach" = q35 ]; then
        # Системный диск на AHCI: путь DMA через bounce-буфер и таблицы команд.
        args+=(-drive "format=raw,file=$DEVDISK,if=none,id=sys,snapshot=on" -device ahci,id=ahci0 -device ide-hd,drive=sys,bus=ahci0.0)
    else
        args+=(-drive "format=raw,file=$DEVDISK,if=none,id=sys,snapshot=on" -device virtio-blk-pci,drive=sys)
    fi
    [ "$fw" = uefi ] && args+=(-device virtio-blk-pci,drive=esp,bootindex=0)
    $QEMU $ACC -machine "$mach" -m 2G -smp "$cpus" "${args[@]}" \
        -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
        -display none -serial "file:$log" -no-reboot \
        -d cpu_reset -D "$WORK/$name.reset" >/dev/null 2>&1 &
    local pid=$! i
    for i in $(seq 1 "$secs"); do
        grep -aqE "$stop" "$log" 2>/dev/null && break
        kill -0 $pid 2>/dev/null || break
        sleep 1
    done
    sleep 3                              # дать дойти строкам после маркера (DHCP)
    kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
    cp "$log" "$OUT_DIR/$name.log" 2>/dev/null
}

has()  { grep -aqE "$2" "$WORK/$1.log"; }
hasnt(){ ! grep -aqE "$2" "$WORK/$1.log"; }

# ESP с другим ядром и/или файлом \kload.at.  esp_make <файл> <ядро|-> <kload|->
esp_make() {
    cp "$ESP" "$1"
    if [ "$2" != - ]; then mcopy -D o -i "$1" "$2" ::/kernel.bin </dev/null || return 1; fi
    if [ "$3" != - ]; then
        printf '%s' "$3" > "$WORK/kload.at"
        mcopy -D o -i "$1" "$WORK/kload.at" ::/kload.at </dev/null || return 1
    fi
}

# Общие проверки «система поднялась целиком».  alive <имя> <ядер> <метка>
alive() {
    local n=$1 cpus=$2 t=$3
    check "$t: reaches the login screen"                 "has $n '$READY'"
    check "$t: no panic, exception or heap exhaustion"   "hasnt $n 'PANIC|Page Fault|EXCEPTION|\\[heap\\] OOM|General Protection'"
    check "$t: all $cpus core(s) are up"                 "has $n '\\[smp\\] cores up: $cpus/$cpus' || { [ $cpus = 1 ] && hasnt $n 'AP online'; }"
    check "$t: system volume mounted from the disk"      "has $n '\\[fat32\\] mounted'"
    check "$t: virtio-net works both ways (DHCP offer)"  "has $n '\\[dhcp\\] (OFFER|ACK)'"
    check "$t: smoke 48 (FDE) and 49 (MIND) pass"        "has $n 'smoke 48\\] self-test PASS' && has $n 'smoke 49\\] done in [0-9]+ ms: PASS'"
    check "$t: smoke 51.5 (JIT machine code) passes"     "has $n 'smoke 51.5\\] done in [0-9]+ ms: PASS'"
    if [ $KVM = 1 ] && [ "$cpus" -ge 2 ] && has $n 'smoke 50\] verdict: READY'; then
        check "$t: smoke 50.1 (a guest on an AP, VMXON/VMCS/EPT by physical address) passes" \
              "has $n 'smoke 50\\.1\\] done in [0-9]+ ms: PASS' && hasnt $n 'smoke 50\\.1\\].*(FAIL|skipped)'"
    else
        skip "$t: smoke 50.1 (needs KVM with nested VT-x and a second core)"
    fi
}

# Где лежит образ: «phys 0x<16 цифр>» из строки [boot] kernel image.
phys_of() { grep -a '\[boot\] kernel image' "$WORK/$1.log" | head -1 | sed -E 's/.*phys 0x([0-9A-Fa-f]+)\.\..*/\1/'; }
hex16()   { printf '%016X' "$1"; }

# ============================================================================
echo "== Phase 0: the image layout =="
# ============================================================================
PH=$(readelf -lW "$KERNEL" | grep LOAD)
check "the kernel has segments linked at 0xffffffff80…"        "echo \"\$PH\" | grep -q '0xffffffff80'"
check "their load addresses start right above 1 MB"            "echo \"\$PH\" | awk '\$3 ~ /ffffffff80/ && \$4 ~ /0x00000000001/' | grep -q ."
check "the entry stub is linked low (VMA = LMA at 1 MB)"       "echo \"\$PH\" | awk '\$3 == \$4 && \$3 ~ /0x0000000000100000/' | grep -q ."
check "ELF entry is the low address of the UEFI stub"          "[ \"\$(readelf -hW $KERNEL | awk '/Entry point/ {print \$4}')\" = \"0x\$(nm $KERNEL | awk '\$3 == \"_efi_start\" {print \$1}' | sed 's/^0*//')\" ]"
check "kernel_main64 lives in the upper half"                  "nm $KERNEL | grep -qE '^ffffffff80[0-9a-f]+ T kernel_main64\$'"
check "no undefined symbols"                                   "! nm $KERNEL | grep -q ' U '"
END_LMA=$(( 0x$(nm "$KERNEL" | awk '$3 == "_kernel_end_lma" {print $1}') ))
echo "    image end (load address): $(printf '0x%X' $END_LMA)"

# ============================================================================
echo "== Phase 1: UEFI, the kernel at different physical addresses =="
# ============================================================================
boot def uefi "$ESP" 4 pc 90 "$READY"
alive def 4 "default place"
P=$(phys_of def)
check "default place: the loader chose a place above the old 8 MB wall (got 0x$P)" "[ -n \"$P\" ] && [ \$((0x$P)) -ge \$((0x800000)) ]"
check "default place: the delta is a multiple of 2 MB"          "[ -n \"$P\" ] && [ \$(( (0x$P - 0x100000) % 0x200000 )) = 0 ]"
check "default place: the image is below 4 GB"                  "[ -n \"$P\" ] && [ \$((0x$P + END_LMA)) -lt \$((0x100000000)) ]"

for D in 0x0 0xA000000 0x40000000; do
    N="at$(printf '%X' $D)"
    esp_make "$WORK/esp-$N.img" - "$D" || { bad "cannot prepare an ESP with kload.at=$D"; continue; }
    boot "$N" uefi "$WORK/esp-$N.img" 4 pc 90 "$READY"
    WANT=$(hex16 $(( D + 0x100000 )))
    check "delta $D: the kernel reports phys 0x$WANT"            "[ \"\$(phys_of $N | tr a-f A-F)\" = \"$WANT\" ]"
    alive "$N" 4 "delta $D"
done

boot one uefi "$ESP" 1 pc 90 "$READY"
alive one 1 "one core"

boot q35 uefi "$ESP" 4 q35 90 "$READY"
check "q35: the system disk is on AHCI"                         "has q35 '\\[ahci\\]'"
alive q35 4 "q35 + AHCI"

# ============================================================================
echo "== Phase 2: the loader refuses a bad place =="
# ============================================================================
esp_make "$WORK/esp-busy.img" - 0x800000
boot busy uefi "$WORK/esp-busy.img" 2 pc 40 'not free|k64\]'
check "delta 0x800000 (firmware memory): the loader says the place is not free" "has busy 'requested place for the kernel is not free'"
check "…and the kernel is not started"                           "hasnt busy '\\[k64\\]'"

esp_make "$WORK/esp-odd.img" - 0x100000
boot odd uefi "$WORK/esp-odd.img" 2 pc 40 'not a multiple|k64\]'
check "delta 0x100000: refused, not a multiple of 2 MB"         "has odd 'not a multiple of 2 MB' && hasnt odd '\\[k64\\]'"

esp_make "$WORK/esp-heap.img" - 0x2000000
boot heap uefi "$WORK/esp-heap.img" 2 pc 40 'heap range|k64\]'
check "delta 0x2000000: refused, inside the kernel heap range"  "has heap 'into the kernel heap range' && hasnt heap '\\[k64\\]'"

if [ "${AOS_38_SKIP_BUILDS:-0}" = 1 ]; then
    skip "phases 3-6 (extra kernels) — AOS_38_SKIP_BUILDS=1"
    echo "== stage 38.1.1 reloc: $PASS passed, $FAIL failed, $SKIP skipped =="
    [ $FAIL = 0 ]; exit $?
fi

# Отдельное ядро в своём каталоге.  build_alt <имя> <доп. параметры make…>
build_alt() {
    local n=$1; shift
    $MK BUILD_DIR=build/x86_64-$n KERNEL=build/kernel-$n.bin "$@" build/kernel-$n.bin >"$WORK/build-$n.log" 2>&1
}
# Маленький ISO для ветки BIOS: GRUB + ядро + initrd, без установщика.
mini_iso() {
    local d="$WORK/iso-$2"
    rm -rf "$d"; mkdir -p "$d/boot/grub"
    cp "$1" "$d/boot/kernel-x86_64.bin"; cp "$INITRD" "$d/boot/initrd.img"
    cat > "$d/boot/grub/grub.cfg" <<EOF
set timeout=0
set default=0
menuentry "AMPLIFY-OS" {
    insmod all_video
    insmod gfxterm
    set gfxmode=1920x1080x32,1280x1024x32,auto
    set gfxpayload=keep
    terminal_output gfxterm
    multiboot2 /boot/kernel-x86_64.bin
    module2 /boot/initrd.img amplify-initrd
    boot
}
EOF
    grub-mkrescue -d /usr/lib/grub/i386-pc -o "$WORK/$2.iso" "$d" >"$WORK/iso-$2.log" 2>&1
}

# ============================================================================
echo "== Phase 3: an image larger than the old 8 MB wall =="
# ============================================================================
check "the bloated kernel (.bss + 8 MB) builds"                 "build_alt bloat CFLAGS_EXTRA=-DAOS_BLOAT_BSS_MB=8"
if [ -f build/kernel-bloat.bin ]; then
    BEND=$(( 0x$(nm build/kernel-bloat.bin | awk '$3 == "_kernel_end_lma" {print $1}') ))
    check "its image ends above 8 MB (at $(printf '0x%X' $BEND))"  "[ $BEND -gt \$((0x800000)) ]"
    check "the build refuses an image above its limit"          "! $MK BUILD_DIR=build/x86_64-bloat KERNEL=build/kernel-bloat2.bin CFLAGS_EXTRA=-DAOS_BLOAT_BSS_MB=8 KERNEL_END_LIMIT=0x800000 build/kernel-bloat2.bin >$WORK/build-limit.log 2>&1 && grep -q 'kernel image ends at' $WORK/build-limit.log && [ ! -f build/kernel-bloat2.bin ]"
    esp_make "$WORK/esp-bloat.img" build/kernel-bloat.bin -
    boot bloat uefi "$WORK/esp-bloat.img" 4 pc 90 "$READY"
    check "bloated: the extra .bss is zeroed and writable"      "has bloat '\\[boot\\] bloat: 8 MB of extra \\.bss, zeroed and writable: OK'"
    alive bloat 4 "bloated image"
    esp_make "$WORK/esp-bloat0.img" build/kernel-bloat.bin 0x0
    boot bloat0 uefi "$WORK/esp-bloat0.img" 2 pc 40 'not free|k64\]'
    check "bloated at the old address (1 MB): refused — it would lie on firmware memory" "has bloat0 'requested place for the kernel is not free' && hasnt bloat0 '\\[k64\\]'"
fi

# ============================================================================
echo "== Phase 4: BIOS + GRUB (Multiboot2, the image at its load address) =="
# ============================================================================
if command -v grub-mkrescue >/dev/null 2>&1 && [ -d /usr/lib/grub/i386-pc ]; then
    check "a BIOS ISO with the kernel builds"                   "mini_iso $KERNEL bios"
    boot bios bios "$WORK/bios.iso" 4 pc 120 "$READY"
    check "BIOS: the kernel sits at 1 MB, delta 0"              "[ \"\$(phys_of bios | tr a-f A-F)\" = \"$(hex16 $((0x100000)))\" ] && has bios 'delta 0x0000000000000000'"
    # Конец свободной области, начинающейся с 1 МБ (в журнале строки с \r).
    BTOP=$(grep -a '\[pmm\] map 1 0x0000100000-' "$WORK/bios.log" | head -1 | sed -E 's/.*-0x([0-9A-Fa-f]+).*/\1/')
    check "BIOS: memory from 1 MB up is one free region past 16 MB — no 8 MB wall here (ends at 0x${BTOP:-?})" \
          "[ -n \"$BTOP\" ] && [ \$((0x${BTOP:-0})) -ge \$((0x1000000)) ]"
    UWALL=$(grep -a '\[pmm\] map 4 0x0000800000-' "$WORK/def.log" | head -1)
    check "UEFI: the same place holds firmware memory (ACPI NVS at 0x800000) — the wall was real" "[ -n \"$UWALL\" ]"
    alive bios 4 "BIOS"
    if [ -f build/kernel-bloat.bin ]; then
        check "a BIOS ISO with the bloated kernel builds"       "mini_iso build/kernel-bloat.bin biosbloat"
        boot biosbloat bios "$WORK/biosbloat.iso" 4 pc 120 "$READY"
        check "BIOS, bloated: extra .bss is fine"               "has biosbloat '\\[boot\\] bloat: 8 MB of extra \\.bss, zeroed and writable: OK'"
        alive biosbloat 4 "BIOS, bloated image"
    fi
else
    skip "BIOS branch (no grub-mkrescue / i386-pc modules here)"
fi

# ============================================================================
echo "== Phase 5: the test can fail (virt_to_phys without the delta) =="
# ============================================================================
check "the mutant kernel builds"                                "build_alt mut CFLAGS_EXTRA=-DAOS_MUT_V2P_NO_DELTA=1"
if [ -f build/kernel-mut.bin ]; then
    esp_make "$WORK/esp-mut0.img" build/kernel-mut.bin 0x0
    boot mut0 uefi "$WORK/esp-mut0.img" 4 pc 90 "$READY"
    check "mutant at delta 0: boots (the error is invisible there)" "has mut0 '$READY' && has mut0 '\\[fat32\\] mounted'"
    esp_make "$WORK/esp-mutA.img" build/kernel-mut.bin 0xA000000
    boot mutA uefi "$WORK/esp-mutA.img" 4 pc 60 "$READY|PANIC|Page Fault"
    check "mutant at delta 0xA000000: does NOT come up clean (devices get wrong addresses)" \
          "hasnt mutA '$READY' || hasnt mutA '\\[fat32\\] mounted' || hasnt mutA '\\[smp\\] cores up: 4/4' || hasnt mutA '\\[dhcp\\] (OFFER|ACK)' || has mutA 'PANIC|Page Fault|EXCEPTION|FAIL'"
fi

# ============================================================================
echo "== Phase 6: the way back (the old low layout, HIGHER_HALF=0) =="
# ============================================================================
check "the low kernel builds"                                   "build_alt low HIGHER_HALF=0"
if [ -f build/kernel-low.bin ]; then
    check "it is linked at 1 MB with VMA = LMA"                 "! readelf -lW build/kernel-low.bin | grep LOAD | grep -q ffffffff"
    esp_make "$WORK/esp-low.img" build/kernel-low.bin -
    boot low uefi "$WORK/esp-low.img" 4 pc 90 "$READY"
    check "low: the loader recognises it and loads at the link address" "has low 'legacy low kernel'"
    check "low: the kernel reports phys 1 MB, virt base 0"      "has low 'kernel image: phys 0x0000000000100000' && has low 'virt base 0x0000000000000000'"
    alive low 4 "low layout"
fi

echo "== stage 38.1.1 reloc: $PASS passed, $FAIL failed, $SKIP skipped =="
[ $FAIL = 0 ]

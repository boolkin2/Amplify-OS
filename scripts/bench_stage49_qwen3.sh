#!/usr/bin/env bash
# ============================================================================
# bench_stage49_qwen3.sh — замер AMPLIFY MIND на Qwen3-1.7B при загрузке ОС.
#   bash scripts/bench_stage49_qwen3.sh <cpus> <mem> "<mind.conf строки через ;>" [machine]
# Пример: bash scripts/bench_stage49_qwen3.sh 4 6G "bench_on_boot=4" q35
# q35 (по умолчанию): ESP и тестовый диск — порты встроенного AHCI (DMA),
# ниже 4 ГБ у q35 только 2 ГБ RAM — веса 1.8 ГБ ложатся в память выше 4 ГБ.
# Ядро после smoke 49 грузит модель, меряет prefill (64 токена системного
# промпта) и генерацию, печатает "[mind] boot bench: ...".
# ============================================================================
cd "$(dirname "$0")/.."
CPUS=${1:-4}; MEM=${2:-6G}; CONF=${3:-"bench_on_boot=4"}; MACH=${4:-q35}
W=build/q49/bench_$CPUS; mkdir -p $W; rm -f $W/serial.log
IMG=build/q49/qdisk.img
if [ ! -f "$IMG" ] || [ -n "${REMAKE:-}" ]; then
    bash scripts/make_qwen3_disk.sh "$IMG" 3G build/mind/Qwen3-1.7B-Q8_0.gguf "$(echo "$CONF" | tr ';' '\n')\n"
else
    # диск уже есть — только mind.conf
    echo "$CONF" | tr ';' '\n' > $W/mind.conf
    MTOOLS_SKIP_CHECK=1 mcopy -D o -i "$IMG" $W/mind.conf ::/Sanctum/mind.conf </dev/null
fi
cp build/esp.img $W/esp.img
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then CPU="-enable-kvm -cpu host"; else CPU="-accel tcg,thread=multi -cpu max"; fi
if [ "$MACH" = q35 ]; then
    DRIVES="-drive file=$IMG,format=raw,if=none,id=d0 -device ide-hd,drive=d0,bus=ide.0
            -drive file=$W/esp.img,format=raw,if=none,id=d1 -device ide-hd,drive=d1,bus=ide.1,bootindex=0"
else
    DRIVES="-drive file=$IMG,format=raw,if=ide,index=0,media=disk -drive file=$W/esp.img,format=raw,if=ide,index=1"
fi
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd $CPU -machine $MACH -m $MEM \
  -smp cpus=$CPUS,cores=$CPUS,threads=1 -vga none -device virtio-vga,xres=1920,yres=1080 \
  -display none -serial file:$W/serial.log -no-reboot -no-shutdown $DRIVES >/dev/null 2>&1 &
QP=$!
for i in $(seq 1 ${T:-3600}); do
  grep -q "\[mind\] boot bench" $W/serial.log 2>/dev/null && break
  grep -q "PANIC\|EXCEPTION" $W/serial.log 2>/dev/null && break
  kill -0 $QP 2>/dev/null || break
  sleep 1
done
kill -9 $QP 2>/dev/null; wait $QP 2>/dev/null
echo "cpus=$CPUS mem=$MEM machine=$MACH conf=$CONF:"
grep -E '\[pmm\]|\[mind\] (load|loaded|candidate|boot bench|error)|smoke 49\] done|smoke 49.1|PANIC|EXCEPTION' $W/serial.log | sed 's/^/  /'

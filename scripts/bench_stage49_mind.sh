#!/usr/bin/env bash
# Замер MIND при загрузке: bash scripts/bench_stage49_mind.sh <cpus> "<mind.conf строки через ;>"
# Кладёт build/mind/SmolLM2-135M-Instruct-Q8_0.gguf на копию dev-диска и
# mind.conf с bench_on_boot=N; ядро после smoke 49 грузит модель, меряет
# prefill/генерацию и печатает "[mind] boot bench: ...". Пример:
#   bash scripts/bench_stage49_mind.sh 4 "bench_on_boot=32;simd=auto"
cd "$(dirname "$0")/.."
CPUS=${1:-4}; CONF=${2:-"bench_on_boot=16"}
W=/tmp/b49_$CPUS; mkdir -p $W; rm -f $W/*
cp --sparse=always build/disk-x86_64.img $W/dev.img
cp build/esp.img $W/esp.img
export MTOOLS_SKIP_CHECK=1
mmd -D s -i $W/dev.img ::/vault ::/vault/mind ::/Sanctum </dev/null 2>/dev/null
mcopy -D o -i $W/dev.img build/mind/SmolLM2-135M-Instruct-Q8_0.gguf ::/vault/mind/ </dev/null
echo "$CONF" | tr ';' '\n' > $W/mind.conf
mcopy -D o -i $W/dev.img $W/mind.conf ::/Sanctum/mind.conf </dev/null
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then CPU="-enable-kvm -cpu host"; else CPU="-accel tcg,thread=multi -cpu max"; fi
qemu-system-x86_64 -bios /usr/share/ovmf/OVMF.fd $CPU -machine pc -m 1G \
  -smp cpus=$CPUS,cores=$CPUS,threads=1 -vga none -device virtio-vga,xres=1920,yres=1080 \
  -display none -serial file:$W/serial.log -no-reboot -no-shutdown \
  -drive file=$W/dev.img,format=raw,if=ide,index=0,media=disk \
  -drive file=$W/esp.img,format=raw,if=ide,index=1 >/dev/null 2>&1 &
QP=$!
for i in $(seq 1 ${T:-400}); do
  grep -q "\[mind\] boot bench" $W/serial.log 2>/dev/null && break
  grep -q "PANIC" $W/serial.log 2>/dev/null && break
  sleep 1
done
kill -9 $QP 2>/dev/null; wait $QP 2>/dev/null
echo "cpus=$CPUS conf=$CONF: $(grep -E '\[mind\] boot bench|PANIC|smoke 49\] done' $W/serial.log | tr '\n' ' ')"
rm -f $W/dev.img $W/esp.img

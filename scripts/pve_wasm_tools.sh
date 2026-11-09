#!/usr/bin/env bash
# ============================================================================
# pve_wasm_tools.sh — инструменты WebAssembly на узле Proxmox (этап 51).
# Запуск с ПК:  bash scripts/pve_run.sh scripts/pve_wasm_tools.sh
# ----------------------------------------------------------------------------
# Пользователь разрешил скачать (07.10): clang, lld, wasi-libc и wabt из
# репозитория Debian и официальный набор тестов WebAssembly с GitHub.
# root у `ai` нет, поэтому пакеты не ставятся, а распаковываются в ~/local
# (как nasm и xorriso раньше): `apt download` + `dpkg -x`. ~/local лежит на
# корневом разделе узла и переживает перезагрузку (в отличие от /tmp/ai-work).
#   clang-19, lld-19 (wasm-ld)      — компилятор C в wasm32-wasi
#   wasi-libc, compiler-rt (wasm32) — библиотека C для WASI
#   wabt                            — wat2wasm, wast2json, wasm-objdump
#   тесты                           — WebAssembly/spec, тег версии 2.0 -> test/core
# Скрипт можно повторять: скачанное и распакованное второй раз не трогается.
# ============================================================================
set -u
. ~/local/env.sh
L=$HOME/local
D=$L/debs/wasm
mkdir -p "$D" "$L/share/wasm-spec"
cd "$D" || exit 1

PKGS="clang-19 lld-19 libclang-common-19-dev libclang-cpp19 libllvm19 libclang-rt-19-dev-wasm32 wasi-libc wabt"
for p in $PKGS; do
    ls ${p}_*.deb >/dev/null 2>&1 || apt download "$p" >/dev/null 2>&1 || echo "download FAILED: $p"
done
# Распаковка — один раз (маркер рядом с пакетом).
for f in *.deb; do
    [ -f "$f.unpacked" ] && continue
    dpkg -x "$f" "$L" && touch "$f.unpacked"
done

CLANG=$L/usr/lib/llvm-19/bin/clang
# Каких разделяемых библиотек не хватает (их нет в системе узла) — докачать.
need() { ldd "$1" 2>/dev/null | grep 'not found' | awk '{print $1}'; }
MISS=$( (need "$CLANG"; need "$L/usr/lib/llvm-19/bin/lld"; need "$L/usr/bin/wat2wasm") | sort -u)
for so in $MISS; do
    case "$so" in
        libz3.so.*)    p=libz3-4;;
        libedit.so.*)  p=libedit2;;
        libxml2.so.*)  p=libxml2;;
        libbsd.so.*)   p=libbsd0;;
        libmd.so.*)    p=libmd0;;
        *) echo "missing library with no known package: $so"; continue;;
    esac
    ls ${p}_*.deb >/dev/null 2>&1 || apt download "$p" >/dev/null 2>&1 || echo "download FAILED: $p"
    for f in ${p}_*.deb; do [ -f "$f.unpacked" ] || { dpkg -x "$f" "$L" && touch "$f.unpacked"; }; done
done
echo "still missing: $( (need "$CLANG"; need "$L/usr/lib/llvm-19/bin/lld"; need "$L/usr/bin/wat2wasm") | sort -u | tr '\n' ' ')"

# Набор тестов: архив исходников спецификации по тегу, из него нужен test/core.
S=$L/share/wasm-spec
if [ ! -f "$S/ref.txt" ]; then
    TAGS=$(curl -s -m 30 'https://api.github.com/repos/WebAssembly/spec/tags?per_page=100' | grep '"name"' | cut -d'"' -f4 | tr '\n' ' ')
    echo "spec tags: $TAGS" | cut -c1-300
    REF=""
    for t in wg-2.0 wg-2.0.draft1 opam-2.0.1 opam-2.0.0; do
        echo " $TAGS " | grep -q " $t " && { REF=$t; break; }
    done
    if [ -n "$REF" ]; then
        curl -sL -m 180 -o "$S/spec.tar.gz" "https://codeload.github.com/WebAssembly/spec/tar.gz/refs/tags/$REF" &&
        tar -xzf "$S/spec.tar.gz" -C "$S" --wildcards '*/test/core/*.wast' --strip-components=3 &&
        echo "$REF" > "$S/ref.txt"
    else
        echo "no 2.0 tag found among the tags above"
    fi
fi
echo "spec ref: $(cat "$S/ref.txt" 2>/dev/null)  wast files: $(ls "$S"/*.wast 2>/dev/null | wc -l)  simd: $(ls "$S"/simd/*.wast "$S"/simd_*.wast 2>/dev/null | wc -l)"

# Проба: программа на C -> wasm32-wasi, и что в ней за команды и импорты.
T=/tmp/ai-wasm-probe; mkdir -p $T
cat > $T/hello.c <<'EOF'
#include <stdio.h>
int main(int argc, char** argv) { printf("hello from wasm, argc=%d\n", argc); return 7; }
EOF
"$CLANG" --target=wasm32-wasi --sysroot="$L/usr" -O2 -o $T/hello.wasm $T/hello.c 2>&1 | head -5
ls -la $T/hello.wasm 2>/dev/null | cut -c1-80
"$CLANG" --version | head -1
wat2wasm --version 2>&1 | head -1
wasm-objdump -x $T/hello.wasm 2>/dev/null | grep -E '^(Import|Export|Type|Function|Memory|Table|Data|Custom)|wasi_snapshot|target_features| - feature' | head -30
du -sh "$L" | cut -f1
df -h ~ | tail -1 | awk '{print "free on /:", $4}'
echo WASM_TOOLS_DONE

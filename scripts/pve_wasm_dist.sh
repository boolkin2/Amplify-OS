#!/usr/bin/env bash
# ============================================================================
# pve_wasm_dist.sh — обновить встроенные программы Rune (userspace/wasm/dist).
#   bash scripts/pve_wasm_dist.sh          (Git Bash, из корня дерева)
# Компилятор в WebAssembly есть только на узле Proxmox, а готовые модули лежат
# в дереве (их кладёт в initrd Makefile). Скрипт: исходники -> узел, там
# `make wasm-dist` и сверка каждой программы с её же сборкой под gcc
# (build/wasm/dist_check.sh), затем каталог dist — обратно на ПК.
# Запускать после правки userspace/wasm/{oracle,abacus,sieve,scribe}.c.
# ============================================================================
set -u
cd "$(dirname "$0")/.."
export PVE_WORK="${PVE_WORK:-/tmp/ai-work}"
bash scripts/pve_sync.sh | tail -1 || exit 1
bash scripts/pve_run.sh scripts/wasm_dist_check.sh || { echo "dist check failed - nothing copied"; exit 1; }
mkdir -p userspace/wasm/dist
for n in oracle abacus sieve scribe; do
    scp.exe -O -q "pve-node:$PVE_WORK/OS2/userspace/wasm/dist/$n.wasm" userspace/wasm/dist/ || exit 1
done
ls -l userspace/wasm/dist

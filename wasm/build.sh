#!/bin/sh
# 編譯 leaf.c 成 leaf.wasm,並印出 base64(貼進 index.html 引擎裡的 WASM_B64)。需要 clang(含 wasm32 目標)與 wasm-ld
set -e
cd "$(dirname "$0")"
clang --target=wasm32 -O3 -nostdlib -fno-builtin -ffp-contract=off -fno-math-errno -Wl,--no-entry -Wl,--strip-all -Wl,--lto-O3 -Wl,-z,stack-size=8192 -Wl,--initial-memory=12582912 -Wl,--max-memory=12582912 -o leaf.wasm leaf.c
wc -c leaf.wasm
base64 -w0 leaf.wasm > leaf.wasm.b64

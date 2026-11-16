#!/usr/bin/env bash
# Build docs/orderbook.wasm from the unmodified book (cpp/src/orderbook.cpp) with plain clang and
# wasm-ld: no Emscripten or libc++ needed. The shim/ headers stand in for the few standard
# library pieces the book uses.
set -euo pipefail
cd "$(dirname "$0")"
OUT="${WASM_OUT:-$(cd ../.. && pwd)/docs/orderbook.wasm}"
FLAGS=(--target=wasm32 -std=c++20 -O2 -ffreestanding -nostdlibinc -nostdinc++ -fno-exceptions -fno-rtti -fno-c++-static-destructors
       -isystem shim -I ../include -Wall -Wno-unused-function)
LD="${WASM_LD:-$(command -v wasm-ld || ls /usr/lib/llvm-*/bin/wasm-ld 2>/dev/null | tail -1)}"
[ -n "$LD" ] || { echo "wasm-ld not found: sudo apt install lld"; exit 1; }
mkdir -p build
clang++ "${FLAGS[@]}" -include prelude.hpp -c ../src/orderbook.cpp -o build/orderbook.o
clang++ "${FLAGS[@]}" -c runtime.cpp -o build/runtime.o
clang++ "${FLAGS[@]}" -include prelude.hpp -c ob_wasm.cpp -o build/ob_wasm.o
"$LD" --no-entry --strip-all --allow-undefined-file=imports.txt -o "$OUT" \
        build/orderbook.o build/runtime.o build/ob_wasm.o
echo "wrote $OUT ($(wc -c < "$OUT") bytes)"

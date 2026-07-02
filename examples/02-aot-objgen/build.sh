#!/usr/bin/env bash
# Build the AOT object generator, run it to emit aot_out.o, then link & run.
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v llvm-config >/dev/null 2>&1; then
  echo "error: llvm-config not on PATH." >&2
  exit 1
fi

cmake -S . -B build -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build

echo "----- generating aot_out.o -----"
./build/aot

echo "----- linking with driver.c and running -----"
cc aot_out.o driver.c -o prog
./prog

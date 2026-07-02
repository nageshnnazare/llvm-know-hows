#!/usr/bin/env bash
# Build and run the LLJIT example.
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v llvm-config >/dev/null 2>&1; then
  echo "error: llvm-config not on PATH." >&2
  exit 1
fi

cmake -S . -B build -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build
echo "----- output -----"
./build/jit

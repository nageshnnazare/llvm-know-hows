#!/usr/bin/env bash
# Build the mul-to-add pass plugin and run it on test.ll via opt.
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v llvm-config >/dev/null 2>&1; then
  echo "error: llvm-config not on PATH." >&2
  exit 1
fi

cmake -S . -B build -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build

# Find the built shared object (.so on Linux, .dylib on macOS).
PLUGIN=$(ls build/MulToAdd.* 2>/dev/null | grep -E '\.(so|dylib)$' | head -n1)
if [ -z "${PLUGIN:-}" ]; then echo "plugin not found in build/"; exit 1; fi

echo "----- running opt -passes=mul-to-add on test.ll -----"
opt -load-pass-plugin="$PLUGIN" -passes=mul-to-add -S test.ll -o -

# 00.05 · Environment Setup & The Toolchain

> Before writing a compiler you need LLVM (and MLIR) installed, a way to link against it,
> and familiarity with the CLI tools. This chapter gets you to a working "hello, IR" build
> on macOS/Linux, plus a reference for every binary in the toolbox.

---

## 1. Getting LLVM + MLIR

You have three options, in increasing order of effort and control:

```
  ┌────────────────────────────────────────────────────────────────────────┐
  │ A. Package manager (fastest, fine for AOT/JIT, sometimes lacks MLIR)   │
  │    macOS:   brew install llvm                                          │
  │    Ubuntu:  sudo apt install llvm-18-dev libclang-18-dev mlir-18-tools │
  │             (or use https://apt.llvm.org for latest)                   │
  ├────────────────────────────────────────────────────────────────────────┤
  │ B. Prebuilt release tarball from github.com/llvm/llvm-project/releases │
  │    Unpack, point CMake/PATH at it. Includes MLIR.                      │
  ├────────────────────────────────────────────────────────────────────────┤
  │ C. Build from source (full control, needed for hacking on LLVM itself, │
  │    guarantees MLIR + all targets). Slow (30 min–2 hr) but definitive.  │
  └────────────────────────────────────────────────────────────────────────┘
```

### Building from source (option C) — the canonical incantation

```bash
>>> git clone --depth=1 https://github.com/llvm/llvm-project.git
>>> cd llvm-project
>>> cmake -S llvm -B build -G Ninja \
        -DLLVM_ENABLE_PROJECTS="clang;lld" \
        -DLLVM_ENABLE_RUNTIMES="compiler-rt" \
        -DLLVM_ENABLE_PROJECTS_USED=ON \
        -DLLVM_TARGETS_TO_BUILD="X86;AArch64;RISCV" \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_ENABLE_ASSERTIONS=ON
>>> # For MLIR, add it under projects too:
>>> cmake -S llvm -B build -G Ninja \
        -DLLVM_ENABLE_PROJECTS="clang;lld;mlir" \
        -DLLVM_TARGETS_TO_BUILD="X86;AArch64" \
        -DCMAKE_BUILD_TYPE=Release \
        -DLLVM_ENABLE_ASSERTIONS=ON
>>> ninja -C build
```

Key flags explained:

```
  LLVM_ENABLE_PROJECTS   which sub-projects to build (clang, lld, mlir, ...)
  LLVM_TARGETS_TO_BUILD  which CPU backends to include. "X86;AArch64" keeps it
                         small; "all" builds every target (bigger, slower).
                         "Native" = just your host CPU.
  CMAKE_BUILD_TYPE       Release = optimized & smaller; Debug = huge but
                         debuggable LLVM internals.
  LLVM_ENABLE_ASSERTIONS ON is strongly recommended while learning: LLVM's
                         internal sanity checks catch your API misuse early.
```

> **Tip:** assertions ON is the difference between "the verifier tells you your IR is
> malformed" and "you get a mysterious segfault deep in codegen." Keep them on.

---

## 2. `llvm-config`: your link-flags oracle

`llvm-config` tells you how to compile and link against the installed LLVM:

```bash
>>> llvm-config --version
18.1.8
>>> llvm-config --cxxflags        # include paths, defines, -std
-I/usr/lib/llvm-18/include -std=c++17 -D__STDC_CONSTANT_MACROS ...
>>> llvm-config --ldflags          # library search paths
-L/usr/lib/llvm-18/lib
>>> llvm-config --libs core orcjit native    # the actual -l flags for given components
-lLLVMCore -lLLVMOrcJIT -lLLVMX86CodeGen ...
>>> llvm-config --system-libs      # transitive system deps (-lz -lpthread ...)
```

A no-CMake one-liner to compile a tiny LLVM program:

```bash
>>> clang++ main.cpp $(llvm-config --cxxflags --ldflags --libs core --system-libs) -o main
```

The `--libs` *components* you pass matter. Common ones:

```
  core         IR data structures (Module, Function, IRBuilder, ...)
  support      utilities (errors, command line, ...)
  native       the host target's backend (expands to e.g. X86CodeGen + X86AsmParser ...)
  orcjit       the ORC JIT framework (section 04)
  passes       the optimization pass infrastructure
  irreader     parse .ll/.bc files
  mcjit        legacy JIT (avoid; use orcjit)
```

---

## 3. CMake setup (the real way to build your compiler)

This is the template every example in this guide uses. `find_package(LLVM)` consumes the
config LLVM installs.

```cmake
cmake_minimum_required(VERSION 3.20)
project(MyCompiler CXX)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")

# Bring in LLVM's include dirs and definitions
include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})

add_executable(mycompiler main.cpp)

# Map LLVM components → concrete library names for this install
llvm_map_components_to_libnames(llvm_libs
    core support irreader
    orcjit native      # add what you need: passes, mcjit, etc.
)
target_link_libraries(mycompiler PRIVATE ${llvm_libs})
target_compile_features(mycompiler PRIVATE cxx_std_17)
```

Configure & build, telling CMake where LLVM is if it isn't on the default path:

```bash
>>> cmake -S . -B build -DLLVM_DIR=$(llvm-config --cmakedir)
>>> cmake --build build
```

`$(llvm-config --cmakedir)` prints the directory containing `LLVMConfig.cmake` — the magic
that makes `find_package(LLVM)` work.

For **MLIR**, add `find_package(MLIR REQUIRED CONFIG)` and link the MLIR libs; details in
[../05-mlir/04-building-a-dialect.md](../05-mlir/04-building-a-dialect.md).

---

## 4. Smoke test: "hello, IR"

Create `hello_ir.cpp` — it builds the `square_plus_one` module from chapter 02 and prints it:

```cpp
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

int main() {
  llvm::LLVMContext ctx;
  auto mod = std::make_unique<llvm::Module>("demo", ctx);
  llvm::IRBuilder<> b(ctx);

  // i32 square_plus_one(i32 x)
  auto *i32 = b.getInt32Ty();
  auto *fty = llvm::FunctionType::get(i32, {i32}, /*vararg=*/false);
  auto *fn  = llvm::Function::Create(fty, llvm::Function::ExternalLinkage,
                                     "square_plus_one", mod.get());
  auto *x = fn->getArg(0);
  x->setName("x");

  auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
  b.SetInsertPoint(entry);
  auto *mul = b.CreateMul(x, x, "mul");
  auto *add = b.CreateAdd(mul, b.getInt32(1), "add");
  b.CreateRet(add);

  llvm::verifyFunction(*fn, &llvm::errs());   // sanity check
  mod->print(llvm::outs(), nullptr);          // dump the .ll
  return 0;
}
```

Build & run:

```bash
>>> clang++ hello_ir.cpp $(llvm-config --cxxflags --ldflags --libs core --system-libs) -o hello_ir
>>> ./hello_ir
; ModuleID = 'demo'
source_filename = "demo"
define i32 @square_plus_one(i32 %x) {
entry:
  %mul = mul i32 %x, %x
  %add = add i32 %mul, 1
  ret i32 %add
}
```

If you see that, your environment is correct and you're ready for everything else. This tiny
program already contains the core moves you'll reuse in AOT, JIT, and MLIR-to-LLVM codegen.

---

## 5. The CLI toolbox — full reference card

You'll use these constantly to *inspect* and *cross-check* what your code does.

```
 ┌──────────────────┬─────────────────────────────────────────────────────────┐
 │ binary           │ purpose / typical use                                   │
 ├──────────────────┼─────────────────────────────────────────────────────────┤
 │ clang/clang++    │ C/C++ frontend & driver. -emit-llvm -S to see IR.       │
 │ opt              │ run passes on IR.  opt -O2 -S in.ll -o out.ll           │
 │                  │ list passes: opt --print-passes                         │
 │ llc              │ IR → asm/obj.  llc -filetype=obj in.ll -o out.o         │
 │ lli              │ JIT-run IR directly.  lli in.ll                         │
 │ llvm-as / -dis   │ .ll ↔ .bc (assemble/disassemble bitcode)                │
 │ llvm-link        │ merge multiple .ll/.bc modules into one                 │
 │ llvm-mc          │ assembler/disassembler at the MC layer                  │
 │ llvm-objdump     │ disassemble object files.  -d for code, -r relocations  │
 │ llvm-nm          │ list symbols in an object/archive                       │
 │ llvm-readobj     │ inspect ELF/Mach-O/COFF structure                       │
 │ lld (ld.lld etc.)│ the LLVM linker                                         │
 │ llvm-dwarfdump   │ inspect DWARF debug info                                │
 │ mlir-opt         │ run MLIR passes/conversions on .mlir                    │
 │ mlir-translate   │ MLIR (llvm dialect) ↔ LLVM IR                           │
 │ mlir-cpu-runner  │ JIT-run an MLIR module (after lowering to llvm dialect) │
 │ FileCheck        │ test-harness pattern matcher (used in LLVM's lit tests) │
 └──────────────────┴─────────────────────────────────────────────────────────┘
```

A useful "see every stage" recipe to keep in your pocket:

```bash
>>> clang -O0 -S -emit-llvm foo.c -o foo.O0.ll      # raw frontend IR
>>> opt   -O2 -S foo.O0.ll        -o foo.O2.ll       # after optimization
>>> llc   -O2 foo.O2.ll           -o foo.s           # assembly for host
>>> llc   -O2 -filetype=obj foo.O2.ll -o foo.o       # object file
>>> llvm-objdump -d foo.o                            # disassemble to verify
```

---

## 6. Version & API stability warnings

LLVM's C++ API is **not** stable across major versions. This guide targets **LLVM 17/18+**.
The biggest things that changed and trip up older tutorials:

```
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ • Opaque pointers (15+): no more i32*; just `ptr`. (See IR essentials.)  │
  │ • New PassManager is the default; the legacy PM is deprecated.           │
  │ • ORCv2 (LLJIT) replaced MCJIT and ORCv1. Old `ExecutionEngine`/MCJIT    │
  │   tutorials are obsolete; use LLJIT / ORC layers.                        │
  │ • Many headers moved; namespaces tightened. Read the release notes when  │
  │   bumping versions.                                                      │
  └──────────────────────────────────────────────────────────────────────────┘
```

When you copy code from a blog and it doesn't compile, **check the LLVM version it targeted
first** — that explains 80% of breakage.

---

## Mental model checkpoint

1. What does `llvm-config --libs core orcjit native` give you and why does it matter?
2. In CMake, what does `find_package(LLVM REQUIRED CONFIG)` need to locate, and how do you
   point it there?
3. Why keep `LLVM_ENABLE_ASSERTIONS=ON` while learning?
4. Which CLI tool corresponds to each pipeline arrow: frontend, optimize, codegen, JIT-run?
5. Name three API changes since "old" LLVM that break copied tutorial code.

Foundations complete. Next, the frontend → [../01-frontend/01-lexer.md](../01-frontend/01-lexer.md)

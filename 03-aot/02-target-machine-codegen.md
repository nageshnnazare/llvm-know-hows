# 03.02 · TargetMachine & Emitting Object Files

> This is the heart of AOT in LLVM: turning an optimized `Module` into a native object file
> on disk. We cover targets, triples, the `TargetMachine`, the data layout contract, and the
> exact API sequence (`addPassesToEmitFile`) that writes a `.o`.

---

## 1. Targets, triples, and registration

A **target** is a backend: x86, AArch64, RISC-V, WASM, etc. LLVM can host many at once. A
**triple** selects one plus its OS/ABI.

```
   target triple = "arch-vendor-os-environment"
                    ────  ─────  ──  ───────────
   examples:
     x86_64-unknown-linux-gnu       Intel/AMD 64, Linux, glibc ABI  → ELF
     aarch64-apple-darwin           Apple Silicon, macOS            → Mach-O
     x86_64-pc-windows-msvc         Intel 64, Windows, MSVC ABI     → COFF
     riscv64-unknown-elf            RISC-V 64, bare metal           → ELF
     wasm32-unknown-unknown         WebAssembly                     → wasm
```

Before you can use a target, you must **register** it. For "the host CPU," LLVM provides
one-call initializers:

```cpp
#include "llvm/Support/TargetSelect.h"

// Register just what's needed to emit code for the HOST machine:
llvm::InitializeNativeTarget();
llvm::InitializeNativeTargetAsmPrinter();   // for emitting asm/obj
llvm::InitializeNativeTargetAsmParser();    // if you parse inline asm

// OR register ALL targets you compiled in (needed for cross-compilation):
llvm::InitializeAllTargetInfos();
llvm::InitializeAllTargets();
llvm::InitializeAllTargetMCs();
llvm::InitializeAllAsmParsers();
llvm::InitializeAllAsmPrinters();
```

```
   Why "register"? LLVM backends self-register into a global registry. The
   Init* calls flip them on. Forgetting these → "Unable to find target for
   triple" at runtime. It's the #1 first-time AOT bug.
```

---

## 2. Looking up a target and building a TargetMachine

```cpp
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

std::string triple = llvm::sys::getDefaultTargetTriple();   // host triple
// (or hard-code "x86_64-unknown-linux-gnu" for cross-compilation)

std::string err;
const llvm::Target *target = llvm::TargetRegistry::lookupTarget(triple, err);
if (!target) { llvm::errs() << err; return; }

llvm::TargetOptions opts;
auto RM = std::optional<llvm::Reloc::Model>(llvm::Reloc::PIC_);  // position-independent
llvm::TargetMachine *TM = target->createTargetMachine(
    triple,
    "generic",          // CPU: "generic", "native", or e.g. "skylake"
    "",                 // feature string: e.g. "+avx2,+fma"
    opts, RM);
```

What a `TargetMachine` *is*:

```
   ┌───────────────────────────────────────────────────────────────────────┐
   │ TargetMachine = the complete description of the machine we target:    │
   │   • instruction set (which opcodes exist)                             │
   │   • register file (how many regs, which classes)                      │
   │   • calling convention / ABI                                          │
   │   • scheduling model (latencies, ports) for instruction scheduling    │
   │   • data layout (sizes/alignments/endianness)                         │
   │ It is the object the backend consults at every codegen decision.      │
   └───────────────────────────────────────────────────────────────────────┘
```

The `CPU` and `features` strings tune *which* instructions are allowed. `"native"` /
`getHostCPUName()` enables everything your build machine supports (great for local AOT, bad
for distributing — the binary may use AVX-512 the user's CPU lacks).

---

## 3. The data layout contract

The `Module` must carry the `TargetMachine`'s data layout, or the optimizer/backend will
make wrong assumptions about pointer size and alignment.

```cpp
module.setDataLayout(TM->createDataLayout());
module.setTargetTriple(triple);
```

```
   WHY THIS MATTERS:
     - mem2reg, GVN, and many passes ask the DataLayout "how big is a pointer?
       what's the alignment of this struct?" to reason about loads/stores.
     - The backend needs it to lay out stack frames and globals.
   Set it RIGHT AFTER creating the module (or right before optimizing). A
   mismatch is silent corruption, not a crash.
```

---

## 4. Emitting an object file: `addPassesToEmitFile`

This is the canonical sequence. The backend is itself a pass pipeline (the *legacy*
PassManager is still used for the codegen/MC pipeline — note this, it surprises people).

```cpp
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/Support/FileSystem.h"

bool emitObjectFile(llvm::Module &M, llvm::TargetMachine *TM,
                    const std::string &outPath) {
  std::error_code EC;
  llvm::raw_fd_ostream dest(outPath, EC, llvm::sys::fs::OF_None);
  if (EC) { llvm::errs() << "cannot open " << outPath << ": " << EC.message(); return false; }

  // The CODEGEN pipeline still uses the legacy PassManager:
  llvm::legacy::PassManager codegenPM;

  auto fileType = llvm::CodeGenFileType::ObjectFile;   // (older: CGFT_ObjectFile)
  if (TM->addPassesToEmitFile(codegenPM, dest, /*DwoOut=*/nullptr, fileType)) {
    llvm::errs() << "TargetMachine can't emit this file type";
    return false;
  }

  codegenPM.run(M);   // runs isel → regalloc → MC → writes bytes to `dest`
  dest.flush();
  return true;
}
```

What `addPassesToEmitFile` wires up under the hood (the backend pipeline from section 06):

```
   addPassesToEmitFile builds:
     IR
      │  ISel (SelectionDAG or GlobalISel): IR → MachineInstr
      ▼
     MachineInstr (virtual registers)
      │  scheduling, register allocation, prologue/epilogue insertion
      ▼
     MachineInstr (physical registers)
      │  MC layer: encode to bytes, build sections, symbols, relocations
      ▼
     object file written to `dest`   (ELF / Mach-O / COFF)
```

`CodeGenFileType` options:

```
   ObjectFile     → .o  (machine code bytes; what you usually want)
   AssemblyFile   → .s  (human-readable asm; for inspection/debugging)
   Null           → run codegen but discard output (for timing/testing)
```

---

## 5. Full minimal AOT codegen function

Putting §1–§4 together — give it an optimized `Module`, get a `.o`:

```cpp
#include "llvm/IR/Module.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

bool moduleToObjectFile(llvm::Module &M, const std::string &out) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();

  auto triple = llvm::sys::getDefaultTargetTriple();
  M.setTargetTriple(triple);

  std::string err;
  const auto *target = llvm::TargetRegistry::lookupTarget(triple, err);
  if (!target) { llvm::errs() << err << "\n"; return false; }

  llvm::TargetOptions opts;
  auto *TM = target->createTargetMachine(
      triple, "generic", "", opts, llvm::Reloc::PIC_);

  M.setDataLayout(TM->createDataLayout());   // MUST match TM

  std::error_code EC;
  llvm::raw_fd_ostream dest(out, EC, llvm::sys::fs::OF_None);
  if (EC) { llvm::errs() << EC.message() << "\n"; return false; }

  llvm::legacy::PassManager pm;
  if (TM->addPassesToEmitFile(pm, dest, nullptr,
                              llvm::CodeGenFileType::ObjectFile)) {
    llvm::errs() << "cannot emit object file\n"; return false;
  }
  pm.run(M);
  dest.flush();
  return true;
}
```

> Note the two different pass managers in a complete AOT compiler:
> **new PM** for the IR optimization pipeline (chapter 02.02), **legacy PM** for the codegen
> pipeline inside `addPassesToEmitFile`. This split is historical; codegen hasn't fully
> migrated. Don't let it confuse you — they operate at different stages.

---

## 6. Cross-compilation

The beauty of the triple abstraction: change the string, target a different machine — *no
code change*. To build for AArch64 Linux from an x86 Mac:

```cpp
llvm::InitializeAllTargetInfos();   // need ALL targets, not just native
llvm::InitializeAllTargets();
llvm::InitializeAllTargetMCs();
llvm::InitializeAllAsmPrinters();

std::string triple = "aarch64-unknown-linux-gnu";   // the only change!
// ... lookupTarget(triple) ... createTargetMachine(triple, "generic", ...)
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ The SAME IR + a different triple = code for a different CPU/OS.         │
   │ This is the N+M payoff from chapter 00.01 made concrete: your frontend  │
   │ targets every CPU LLVM supports, for free, by swapping one string.      │
   └─────────────────────────────────────────────────────────────────────────┘
```

(You still need the target's *libraries and linker* to produce a final executable — cross
codegen is solved, cross *linking* needs a sysroot. See next chapter.)

---

## 7. Inspecting the result

```bash
>>> llvm-objdump -d square.o          # disassemble the machine code
>>> llvm-nm square.o                   # list symbols (T=text/defined, U=undefined)
>>> llvm-readobj --sections square.o   # ELF/Mach-O section layout
>>> file square.o                      # confirm format/arch
square.o: ELF 64-bit LSB relocatable, x86-64, ...
```

Seeing `U puts` in `llvm-nm` output, for example, tells you the object *references* `puts`
but doesn't define it — the linker must supply it from libc. That's the bridge to the next
chapter.

---

## Mental model checkpoint

1. What three (or six) `Initialize*` calls are required, and what's the symptom of forgetting
   them?
2. What does a `TargetMachine` encapsulate? Name four things.
3. Why must you call `M.setDataLayout(TM->createDataLayout())`, and what breaks if you don't?
4. Which pass manager does `addPassesToEmitFile` use, and which does IR optimization use?
5. List the stages `addPassesToEmitFile` wires up from IR to object bytes.
6. How do you cross-compile to a different architecture, and what extra non-LLVM things do
   you still need?
7. What does a `U` symbol in `llvm-nm` output mean?

Next → [03-building-an-aot-compiler.md](03-building-an-aot-compiler.md)

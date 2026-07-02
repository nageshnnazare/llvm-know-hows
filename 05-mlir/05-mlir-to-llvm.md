# 05.05 · From MLIR to LLVM IR — and then AOT or JIT

> This is the chapter that closes the loop. Once your MLIR is fully lowered to the **LLVM
> dialect**, you *translate* it into actual LLVM IR (an `llvm::Module`), and from there you
> use the *exact* AOT (section 03) and JIT (section 04) machinery you already built. MLIR is
> the upper floors; LLVM IR is where the elevator meets the ground you know.

---

## 1. The LLVM dialect is not LLVM IR (but mirrors it)

A subtle but vital distinction:

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ The `llvm` DIALECT is MLIR ops (llvm.add, llvm.call, llvm.br, ...) that │
   │ MODEL LLVM IR within MLIR's world (regions, MLIRContext, ops).          │
   │                                                                         │
   │ LLVM IR is the actual llvm::Module / llvm::Instruction in LLVMContext.  │
   │                                                                         │
   │ TRANSLATION converts the former into the latter. It is a near-1:1,      │
   │ mechanical mapping — NOT an optimization, just a representation change. │
   └─────────────────────────────────────────────────────────────────────────┘
```

```
   MLIR llvm dialect                    LLVM IR
   ──────────────────                   ─────────────
   llvm.func @f(%x: i32) -> i32 {  ──▶  define i32 @f(i32 %x) {
     %0 = llvm.add %x, %x : i32           %0 = add i32 %x, %x
     llvm.return %0 : i32                 ret i32 %0
   }                                    }
```

So the pipeline is: lower *to* the llvm dialect (chapter 05.03), then *translate* the llvm
dialect to LLVM IR.

```
   high dialects ──[dialect conversion, ch.05.03]──▶ llvm dialect
                                                         │
                                                    [translation]
                                                         ▼
                                                   llvm::Module (real LLVM IR)
                                                         │
                                          ┌──────────────┴──────────────┐
                                          ▼                             ▼
                                   AOT (section 03)              JIT (section 04)
```

---

## 2. Translating in C++

The function `translateModuleToLLVMIR` does the conversion. You must register the LLVM
dialect's *translation interfaces* first so each llvm-dialect op knows how to emit itself.

```cpp
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"          // translateModuleToLLVMIR
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
using namespace mlir;

std::unique_ptr<llvm::Module>
toLLVMIR(ModuleOp mlirModule, llvm::LLVMContext &llvmCtx) {
  // 1. Register translation interfaces (which dialects can translate themselves).
  registerBuiltinDialectTranslation(*mlirModule->getContext());
  registerLLVMDialectTranslation(*mlirModule->getContext());

  // 2. Translate: MLIR (llvm dialect) → llvm::Module.
  std::unique_ptr<llvm::Module> llvmModule =
      translateModuleToLLVMIR(mlirModule, llvmCtx, "my_module");
  if (!llvmModule) { llvm::errs() << "translation failed\n"; return nullptr; }
  return llvmModule;
}
```

```
   PRECONDITION: mlirModule must contain ONLY llvm-dialect (and builtin) ops.
   If any arith/scf/func/memref op remains, translation FAILS — which is why
   chapter 05.03's full-conversion + reconcile-unrealized-casts matters: it
   guarantees you actually reached the llvm dialect before you get here.
```

On the CLI, the same step:

```bash
>>> mlir-translate lowered.mlir --mlir-to-llvmir -o out.ll
>>> cat out.ll          # real LLVM IR — feed to opt/llc/clang as in section 02/03
```

---

## 3. The complete MLIR → native pipeline (CLI)

End to end, from a mixed-dialect `.mlir` to a running binary, reusing everything from
sections 02–03:

```bash
# 1. Lower MLIR dialects down to the llvm dialect (ch.05.03):
>>> mlir-opt input.mlir \
      -convert-scf-to-cf -convert-arith-to-llvm -convert-func-to-llvm \
      -finalize-memref-to-llvm -convert-cf-to-llvm \
      -reconcile-unrealized-casts \
      -o llvm-dialect.mlir

# 2. Translate llvm dialect → LLVM IR (this chapter):
>>> mlir-translate llvm-dialect.mlir --mlir-to-llvmir -o out.ll

# 3. From here it's IDENTICAL to the LLVM flow you already know (section 02/03):
>>> opt -O2 -S out.ll -o out.opt.ll          # optimize (ch.02.02/02.03)
>>> llc -filetype=obj out.opt.ll -o out.o     # backend → object (ch.03.02)
>>> clang out.o -o program                     # link (ch.03.03/03.04)
>>> ./program                                   # run (AOT!)
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ Steps 3+ are LITERALLY the AOT pipeline from section 03. MLIR added     │
   │ steps 1–2 (high-level modeling + lowering) on top. The destination and  │
   │ the bottom half of the journey are unchanged. THIS is why we built      │
   │ sections 02–04 first: MLIR plugs into them.                             │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 4. JIT-ing MLIR directly: `mlir::ExecutionEngine`

MLIR ships a convenience JIT that wraps everything (lower-to-LLVM-IR + ORC from section 04)
so you can compile-and-run an MLIR module in process without leaving C++.

```cpp
#include "mlir/ExecutionEngine/ExecutionEngine.h"
#include "mlir/ExecutionEngine/OptUtils.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h"
#include "llvm/Support/TargetSelect.h"
using namespace mlir;

int runMLIR(ModuleOp module /* already lowered to llvm dialect */) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  registerBuiltinDialectTranslation(*module->getContext());
  registerLLVMDialectTranslation(*module->getContext());

  // Optional: an IR optimization transformer (the -O2 pipeline, ch.02.02).
  auto optPipeline = makeOptimizingTransformer(/*optLevel=*/2, /*sizeLevel=*/0,
                                               /*targetMachine=*/nullptr);
  ExecutionEngineOptions opts;
  opts.transformer = optPipeline;

  auto maybeEngine = ExecutionEngine::create(module, opts);
  if (!maybeEngine) { llvm::errs() << "engine create failed\n"; return 1; }
  auto &engine = maybeEngine.get();

  // Invoke an exported function named "main" returning i32 (packed-call ABI).
  int result = 0;
  if (engine->invoke("main", result)) { llvm::errs() << "invoke failed\n"; return 1; }
  return result;
}
```

```
   mlir::ExecutionEngine internally:
     module (llvm dialect) ─▶ translateModuleToLLVMIR ─▶ llvm::Module
                            ─▶ (your opt transformer) ─▶ ORC LLJIT (section 04!)
                            ─▶ invoke(name, args...) ─▶ runs native code
   It's the MLIR-flavored front door to the very JIT you built in section 04.
```

The CLI equivalent is `mlir-cpu-runner`:

```bash
>>> mlir-cpu-runner llvm-dialect.mlir -e main -entry-point-result=i32 \
      -shared-libs=/path/to/libmlir_runner_utils.so
```

---

## 5. The full tower, one diagram

Here is the entire guide in a single picture — every section, connected:

```
   SOURCE (your language)
        │ lexer/parser (section 01)
        ▼
   AST
        │ codegen — choose your entry altitude:
        ├──────────────────────────┐
        ▼                          ▼
   LLVM IR directly           MLIR high dialects (section 05)
   (section 01.03)                 │ MLIR passes + progressive lowering (05.03)
        │                          ▼
        │                     MLIR llvm dialect
        │                          │ translateModuleToLLVMIR (05.05)
        │                          ▼
        └────────────────────▶ LLVM IR (llvm::Module)
                                   │ optimize: new PassManager (section 02)
                                   ▼
                              optimized LLVM IR
                                   │ backend: TargetMachine / SelectionDAG (section 06)
                       ┌───────────┴────────────┐
                       ▼                        ▼
              AOT (section 03)           JIT (section 04)
              .o → link → exe            ORC → memory → call
                       │                        │
                       ▼                        ▼
              run later (+BOLT, 03.05)    run now (+tiering, 04.04)
```

```
   ┌──────────────────────────────────────────────────────────────────────────┐
   │ EVERYTHING converges on "optimized LLVM IR," then forks to AOT or JIT.   │
   │ MLIR is an optional, powerful ON-RAMP that lets you start higher and     │
   │ lower gradually. Whether you enter at LLVM IR or at an MLIR dialect, you │
   │ end at the same backend and the same two delivery mechanisms.            │
   └──────────────────────────────────────────────────────────────────────────┘
```

---

## 6. When to use MLIR vs go straight to LLVM IR

```
  ┌─────────────────────────────────────────┬────────────────────────────────────┐
  │ Use MLIR when...                        │ Go straight to LLVM IR when...     │
  ├─────────────────────────────────────────┼────────────────────────────────────┤
  │ • Your domain has high-level structure  │ • Scalar/imperative language whose │
  │   worth optimizing (tensors, loops,     │   semantics map cleanly to LLVM IR │
  │   dataflow, hardware).                  │   (like our Toy language).         │
  │ • You want multiple lowering targets    │ • You want minimal dependencies &  │
  │   (CPU, GPU, accelerators) from one IR. │   the simplest possible toolchain. │
  │ • You'd otherwise invent your own IR    │ • You're learning / prototyping a  │
  │   + pass infra (MLIR gives it free).    │   classic compiler.                │
  │ • ML / HPC / DSL compilers.             │ • A small/medium general-purpose   │
  │                                         │   language.                        │
  └─────────────────────────────────────────┴────────────────────────────────────┘
```

```
   Rule of thumb: if you find yourself wishing LLVM IR had a higher-level
   concept you could optimize before destroying it, that's the signal to add
   an MLIR dialect above it. Otherwise, LLVM IR directly is simpler.
```

---

## Mental model checkpoint

1. Distinguish the *llvm dialect* from *LLVM IR*. What converts one to the other?
2. What is the precondition for `translateModuleToLLVMIR` to succeed, and which earlier step
   guarantees it?
3. Write the CLI sequence from a mixed-dialect `.mlir` to a running native binary.
4. Which steps of that sequence are identical to section 03's AOT flow?
5. What does `mlir::ExecutionEngine` wrap, and which section's machinery does it reuse?
6. In the "full tower" diagram, what do all paths converge on before forking?
7. Give two situations where MLIR is the right choice and two where direct LLVM IR is better.

MLIR complete. Next, the backend deep dive → [../06-backend/01-codegen-pipeline.md](../06-backend/01-codegen-pipeline.md)

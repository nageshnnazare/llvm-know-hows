# The LLVM Compiler Engineering Handbook

### A one-stop, deeply-explained guide to building **AOT**, **JIT**, and **MLIR** compilers

> Goal: take you from "I know C++ and roughly what a compiler is" to "I can design and
> implement a real ahead-of-time compiler, a just-in-time compiler, and an MLIR-based
> compiler on top of LLVM, and explain *why* every piece exists."

This handbook is opinionated about **understanding**. Every concept is paired with ASCII
diagrams, worked examples, and the actual LLVM/MLIR C++ APIs you will type. Nothing is
hand-waved. When something is subtle (SSA, PHI nodes, ORC resource tracking, dialect
conversion legality) we slow down and draw it.

---

## How to read this guide

```
                          ┌────────────────────────────────┐
                          │  00 · FOUNDATIONS              │
                          │  mental model, IR, SSA, setup  │
                          └───────────────┬────────────────┘
                                          │
              ┌───────────────────────────┼───────────────────────────┐
              ▼                           ▼                           ▼
   ┌────────────────────┐     ┌────────────────────────┐   ┌────────────────────┐
   │ 01 · FRONTEND      │     │ 02 · IR & PASSES       │   │ 06 · BACKEND       │
   │ lexer→parser→AST   │────▶│ IRBuilder, opt passes  │──▶│ isel, regalloc,    │
   │ →LLVM IR           │     │ pass pipeline          │   │ machine code       │
   └────────────────────┘     └───────────┬────────────┘   └────────────────────┘
                                          │
              ┌───────────────────────────┼───────────────────────────┐
              ▼                           ▼                           ▼
   ┌────────────────────┐     ┌────────────────────────┐   ┌────────────────────┐
   │ 03 · AOT           │     │ 04 · JIT               │   │ 05 · MLIR          │
   │ emit .o / binaries │     │ ORC v2, compile on     │   │ dialects, lowering │
   │ link & run         │     │ demand, run in-proc    │   │ MLIR→LLVM→native   │
   └────────────────────┘     └────────────────────────┘   └────────────────────┘
                                          │
                                          ▼
                          ┌──────────────────────────────┐
                          │  07 · CAPSTONE               │
                          │  one language, 3 backends:   │
                          │  AOT + JIT + MLIR            │
                          └──────────────────────────────┘
```

**Recommended path:** read `00` fully. Then `01` and `02` (they feed everything). After
that, the three pillars (`03 AOT`, `04 JIT`, `05 MLIR`) are independent — read in any
order. `06 Backend` deepens what `03` glosses over. `07` ties it all together.

---

## Table of contents

### 00 · Foundations
| File | What you learn |
|------|----------------|
| [01-llvm-mental-model.md](00-foundations/01-llvm-mental-model.md) | What LLVM *is*: a library, not a compiler. The three-phase design. Where AOT/JIT/MLIR sit. |
| [02-compilation-pipeline.md](00-foundations/02-compilation-pipeline.md) | Source → tokens → AST → IR → opt → machine code, end to end, with the exact tools at each stage. |
| [03-llvm-ir-essentials.md](00-foundations/03-llvm-ir-essentials.md) | Reading and writing LLVM IR fluently: modules, functions, basic blocks, instructions, types. |
| [04-ssa-and-cfg.md](00-foundations/04-ssa-and-cfg.md) | SSA form, dominance, PHI nodes, control-flow graphs — the core data structure of every optimizer. |
| [05-environment-setup.md](00-foundations/05-environment-setup.md) | Installing/building LLVM + MLIR, linking with CMake, the toolchain binaries (`opt`, `llc`, `clang`, `mlir-opt`). |

### 01 · Frontend (source → IR)
| File | What you learn |
|------|----------------|
| [01-lexer.md](01-frontend/01-lexer.md) | Turning characters into tokens. A complete hand-written lexer. |
| [02-parser-ast.md](01-frontend/02-parser-ast.md) | Recursive-descent + Pratt parsing, building an AST, expression precedence. |
| [03-ast-to-ir.md](01-frontend/03-ast-to-ir.md) | The codegen visitor: lowering AST nodes to LLVM IR with `IRBuilder`. |

### 02 · IR & Passes
| File | What you learn |
|------|----------------|
| [01-ir-builder.md](02-ir-and-passes/01-ir-builder.md) | The `IRBuilder` API in depth: every instruction you'll emit, mem2reg, allocas. |
| [02-optimization-passes.md](02-ir-and-passes/02-optimization-passes.md) | What passes do, analysis vs transform, writing your own pass. |
| [03-pass-pipeline.md](02-ir-and-passes/03-pass-pipeline.md) | The new PassManager, `-O0..-O3`, pipeline construction. |

### 03 · AOT compilation
| File | What you learn |
|------|----------------|
| [01-aot-theory.md](03-aot/01-aot-theory.md) | What "ahead of time" means, the static compilation model, trade-offs. |
| [02-target-machine-codegen.md](03-aot/02-target-machine-codegen.md) | `TargetMachine`, data layout, triples, emitting object files from IR. |
| [03-building-an-aot-compiler.md](03-aot/03-building-an-aot-compiler.md) | A full AOT driver: IR → `.o` → link → executable, with code. |
| [04-linking-and-runtime.md](03-aot/04-linking-and-runtime.md) | Object files, relocations, linking, the runtime/libc, static vs dynamic. |
| [05-bolt-and-post-link.md](03-aot/05-bolt-and-post-link.md) | BOLT, Propeller, PGO/AutoFDO/CSSPGO, LTO, Polly — post-link & profile-guided optimization. |

### 04 · JIT compilation
| File | What you learn |
|------|----------------|
| [01-jit-theory.md](04-jit/01-jit-theory.md) | Why JIT, the in-process execution model, AOT vs JIT trade-offs. |
| [02-orc-architecture.md](04-jit/02-orc-architecture.md) | ORC v2 internals: layers, `JITDylib`, symbol resolution, lazy compilation. |
| [03-building-a-jit.md](04-jit/03-building-a-jit.md) | A full JIT with `LLJIT`, then a hand-rolled JIT from ORC layers. |
| [04-advanced-jit.md](04-jit/04-advanced-jit.md) | Lazy/on-request compilation, speculation, reoptimization, remote JIT. |

### 05 · MLIR
| File | What you learn |
|------|----------------|
| [01-mlir-theory.md](05-mlir/01-mlir-theory.md) | Why MLIR exists, the multi-level idea, regions/blocks/ops, the SSA-CFG-but-nested model. |
| [02-dialects-and-ops.md](05-mlir/02-dialects-and-ops.md) | Dialects, operations, attributes, types, traits, the built-in dialects you'll use. |
| [03-lowering-and-conversion.md](05-mlir/03-lowering-and-conversion.md) | Rewrite patterns, dialect conversion, legality, progressive lowering. |
| [04-building-a-dialect.md](05-mlir/04-building-a-dialect.md) | TableGen (ODS), defining ops/types, a complete toy dialect. |
| [05-mlir-to-llvm.md](05-mlir/05-mlir-to-llvm.md) | Lowering to the LLVM dialect, translating to LLVM IR, then AOT/JIT. |

### 06 · Backend (deep dive)
| File | What you learn |
|------|----------------|
| [01-codegen-pipeline.md](06-backend/01-codegen-pipeline.md) | LLVM IR → SelectionDAG → MachineInstr → MC → bytes. |
| [02-instruction-selection.md](06-backend/02-instruction-selection.md) | SelectionDAG, GlobalISel, pattern matching, legalization. |
| [03-register-allocation.md](06-backend/03-register-allocation.md) | Live ranges, interference, spilling, the greedy allocator. |

### 07 · Capstone
| File | What you learn |
|------|----------------|
| [01-one-language-three-backends.md](07-capstone/01-one-language-three-backends.md) | Take the toy language and ship it as AOT, JIT, and via MLIR. Compare. |

### examples/
Buildable, runnable code referenced throughout. Each has a `README.md`, `CMakeLists.txt`,
and `build.sh`. See [examples/README.md](examples/README.md).

---

## The single most important diagram in this whole guide

Everything below is a variation on this. LLVM is a **library for the middle and back of a
compiler**. You bring (or generate) IR; LLVM optimizes it and turns it into something
runnable. The *only* difference between AOT, JIT, and MLIR is **when** and **where** that
last step happens.

```
            YOU WRITE                    LLVM PROVIDES                   RESULT
    ┌───────────────────────┐   ┌──────────────────────────┐   ┌──────────────────┐
    │ Frontend              │   │ Optimizer (passes)       │   │                  │
    │  lexer/parser/AST     │   │  + Backend (codegen)     │   │                  │
    │  → LLVM IR            │──▶│                          │──▶│                  │
    └───────────────────────┘   └──────────────────────────┘   └──────────────────┘
                                              │
            ┌─────────────────────────────────┼─────────────────────────────────┐
            ▼                                 ▼                                 ▼
     AOT: write bytes to              JIT: write bytes into             MLIR: start *above*
     a .o file on disk,               this process's memory,            LLVM IR, lower through
     link later, run as a             mark executable, call             dialects, eventually
     separate program.                the function pointer now.         emit LLVM IR → AOT/JIT.

     "compile now, run later"         "compile now, run now,            "model your domain at
                                       in the same process"             the right abstraction"
```

Keep coming back to this. If you ever feel lost, ask: *which box am I in, and when does
codegen happen?*

---

## Conventions used in this guide

- **ASCII diagrams** are everywhere. They are the point.
- Code that exists in LLVM/MLIR is shown as it really is. Code you'd write is marked.
- `>>>` in a shell block is a command you run; lines without it are output.
- LLVM version target: **LLVM 17/18+** APIs (ORCv2, new PassManager, opaque pointers).
  Notes are added where older versions differ.
- Every chapter ends with **"Mental model checkpoint"** — if you can answer those, move on.

Start here → [00-foundations/01-llvm-mental-model.md](00-foundations/01-llvm-mental-model.md)

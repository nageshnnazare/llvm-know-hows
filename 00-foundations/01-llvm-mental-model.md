# 00.01 · The LLVM Mental Model

> If you internalize one thing from this entire handbook, make it this chapter.
> Everything else is detail hanging off this skeleton.

---

## 1. LLVM is *not* a compiler. It is a *library for building* compilers.

When people say "compile with LLVM" they usually mean `clang`. But `clang` is just *one*
program built *on top of* the LLVM libraries. LLVM itself is a toolbox:

```
            ┌────────────────────────────────────────────────────────────┐
            │                    "LLVM" the project                      │
            │                                                            │
            │   ┌──────────┐   ┌──────────┐   ┌──────────┐   ┌─────────┐ │
            │   │ libLLVM  │   │  Clang   │   │   LLD    │   │  MLIR   │ │
            │   │ Core     │   │ (C/C++   │   │ (linker) │   │ (multi- │ │
            │   │ (IR, opt,│   │ frontend)│   │          │   │ level   │ │
            │   │  codegen)│   │          │   │          │   │ IR)     │ │
            │   └──────────┘   └──────────┘   └──────────┘   └─────────┘ │
            │        ▲                                                   │
            │        │ you link against this to build YOUR compiler      │
            └────────┼───────────────────────────────────────────────────┘
                     │
              ┌──────┴────────┐
              │ your_compiler │   ← you, in this guide
              └───────────────┘
```

You will `#include "llvm/..."` headers and link `libLLVM`. You become the author of a
compiler the same way `clang`'s authors did: by *calling LLVM's APIs*.

This is liberating. You don't implement register allocation, instruction scheduling, x86
encoding, or 200 optimization passes. **You implement a frontend that produces LLVM IR,
and LLVM does the hard part.**

---

## 2. The classic three-phase design

Every serious compiler since the 1970s has this shape, and LLVM is the textbook example:

```
  ┌────────────┐        ┌─────────────────────┐       ┌────────────┐
  │  FRONTEND  │        │     MIDDLE END      │       │  BACKEND   │
  │            │  IR    │   (the "optimizer") │  IR   │            │
  │ source ───▶│───────▶│   passes transform  │──────▶│ IR ──▶ asm │
  │ → AST → IR │        │   IR → better IR    │       │ → object   │
  └────────────┘        └─────────────────────┘       └────────────┘
   language-specific      language- & target-          target-specific
                          INDEPENDENT
```

Why split it this way? Because of a combinatorial explosion you avoid:

```
   WITHOUT a common IR:                 WITH LLVM IR in the middle:

    C ─┐                                 C ────┐
   C++─┤   ╲ ╱ ╲ ╱                       C++ ──┤
   Rust┤    ╳   ╳   → N×M                Rust──┼──▶ [LLVM IR] ──┬──▶ x86
  Swift┤   ╱ ╲ ╱ ╲     backends          Swift─┤                ├──▶ ARM
  ... ─┘  x86 ARM RISCV                  ... ──┘                └──▶ RISCV

   N frontends × M backends             N frontends + M backends
   = you write N×M things               = you write N+M things
```

A new language only has to emit LLVM IR to instantly get every CPU LLVM supports. A new CPU
backend instantly serves every language. **LLVM IR is the contract in the middle.** This is
*the* reason LLVM won.

---

## 3. LLVM IR: the universal currency

LLVM IR is a low-level, typed, SSA-based, RISC-like virtual instruction set. It exists in
three *isomorphic* forms (same content, different encoding):

```
  ┌──────────────────────┐   ┌───────────────────────┐   ┌───────────────────────┐
  │ 1. In-memory C++     │   │ 2. Textual (.ll)      │   │ 3. Bitcode (.bc)      │
  │    objects           │   │    human-readable     │   │    compact binary     │
  │  llvm::Module        │   │  define i32 @add(...) │   │  0x42 0x43 0xC0 ...   │
  │  llvm::Function      │◀─▶│  ...                  │◀─▶│  (serialized form)    │
  │  llvm::Instruction   │   │  ret i32 %sum         │   │                       │
  └──────────────────────┘   └───────────────────────┘   └───────────────────────┘
         what your code            what you print          what you cache/ship
         manipulates               while debugging         between tools
```

A trivial example. The C function:

```c
int add(int a, int b) { return a + b; }
```

Becomes this LLVM IR (textual form):

```llvm
define i32 @add(i32 %a, i32 %b) {
entry:
  %sum = add i32 %a, %b
  ret i32 %sum
}
```

Read it: define a function returning `i32` named `@add` taking two `i32`s; in the block
`entry`, compute `%sum = a + b`, then return `%sum`. The `%` prefix = local value,
`@` prefix = global symbol. Notice it's *typed* (`i32` everywhere) and looks like clean
assembly. We dissect IR fully in [03-llvm-ir-essentials.md](03-llvm-ir-essentials.md).

---

## 4. Where AOT, JIT, and MLIR fit

Here's the unifying picture. All three start from the *same* idea (produce IR, let LLVM
optimize and codegen) and differ only in the **final delivery mechanism**.

```
                              ┌─────────────────────┐
   your frontend ───────────▶ │      LLVM IR        │
   (or MLIR lowering)         └──────────┬──────────┘
                                         │  optimization passes (shared)
                                         ▼
                              ┌─────────────────────┐
                              │   optimized LLVM IR │
                              └──────────┬──────────┘
                                         │  backend / codegen (shared engine)
                  ┌──────────────────────┼──────────────────────┐
                  ▼                      ▼                      ▼
        ┌───────────────────┐  ┌───────────────────┐  ┌───────────────────┐
        │      AOT          │  │       JIT         │  │  (MLIR feeds in   │
        │ emit object file  │  │ emit machine code │  │   ABOVE LLVM IR)  │
        │ to DISK           │  │ into MEMORY       │  │                   │
        │ ↓                 │  │ ↓                 │  │ MLIR dialects ──▶ │
        │ linker → binary   │  │ fixup → fn ptr    │  │ LLVM dialect  ──▶ │
        │ ↓                 │  │ ↓                 │  │ LLVM IR ──▶ AOT   │
        │ run later, maybe  │  │ call it NOW in    │  │           or JIT  │
        │ on another machine│  │ this process      │  │                   │
        └───────────────────┘  └───────────────────┘  └───────────────────┘
```

- **AOT** (Ahead-Of-Time): the classic model. Compile the whole program *now*, write a
  native object file/executable to disk, run it later. This is what `clang`, `rustc`, `gcc`
  do. Covered in section [03](../03-aot/01-aot-theory.md).

- **JIT** (Just-In-Time): compile to machine code *into the running process's memory*, then
  immediately jump to it via a function pointer. Used by language runtimes (the JVM,
  JavaScript engines), REPLs, and dynamic codegen. LLVM's JIT framework is **ORC**. Covered
  in section [04](../04-jit/01-jit-theory.md).

- **MLIR** (Multi-Level IR): a *separate but related* framework. It lets you define your own
  IRs ("dialects") at higher levels of abstraction (e.g., tensor ops for ML, loop nests),
  then *progressively lower* them down — eventually reaching the **LLVM dialect**, which
  translates to ordinary LLVM IR, which then goes AOT or JIT. MLIR is "LLVM's idea applied
  recursively at many abstraction levels." Covered in section [05](../05-mlir/01-mlir-theory.md).

The crucial insight: **AOT and JIT share the exact same optimizer and codegen.** They are
two thin wrappers around one engine. MLIR sits one floor *up*, generating the IR that then
flows into either.

---

## 5. The components you'll actually touch

```
  ┌─────────────────────────────────────────────────────────────────────────┐
  │ LLVMContext   owns types/constants, NOT thread-safe; one per thread     │
  ├─────────────────────────────────────────────────────────────────────────┤
  │ Module        a translation unit: holds functions + globals             │
  ├─────────────────────────────────────────────────────────────────────────┤
  │ Function      a function: holds basic blocks + arguments                │
  ├─────────────────────────────────────────────────────────────────────────┤
  │ BasicBlock    a straight-line run of instructions ending in 1 terminator│
  ├─────────────────────────────────────────────────────────────────────────┤
  │ Instruction   add, load, store, call, br, ret, ...                      │
  ├─────────────────────────────────────────────────────────────────────────┤
  │ IRBuilder<>   the ergonomic API you use to CREATE instructions          │
  ├─────────────────────────────────────────────────────────────────────────┤
  │ Type / Value  everything is typed; Value = anything with a result       │
  └─────────────────────────────────────────────────────────────────────────┘
```

Containment hierarchy (memorize this — it's the spine of the API):

```
   LLVMContext
       │ owns
       ▼
    Module  ──────────────┐
       │ contains         │ contains
       ▼                  ▼
    Function          GlobalVariable
       │ contains
       ▼
    BasicBlock
       │ contains
       ▼
    Instruction  ──── is-a ───▶ Value  (it produces a result other instrs can use)
```

> **Value is the master abstraction.** A `Value` is "anything that can be used as an
> operand": an instruction's result, a function argument, a constant, a global. Use-def
> chains (who uses whom) are built on `Value`. We'll lean on this constantly.

---

## 6. Why this design enables all three modes so cheaply

Because the frontend and optimizer don't know or care what happens at the end:

```
   frontend & passes:  "here is some optimized IR, a Module."
                                    │
                  ┌─────────────────┴───────────────────┐
                  │ The Module is just data. What you   │
                  │ DO with it is your choice:          │
                  │                                     │
                  │  • hand it to TargetMachine →       │
                  │    write .o   ............. AOT     │
                  │  • hand it to an ORC JIT →          │
                  │    get a fn ptr ........... JIT     │
                  │  • it CAME from MLIR lowering       │
                  │    ........................ MLIR    │
                  └─────────────────────────────────────┘
```

This is why, once you can produce a `Module`, you can target any of the three with a small
amount of additional code. The bulk of your effort (frontend + understanding IR) is shared.

---

## Mental model checkpoint

You should be able to answer these before moving on:

1. Why is LLVM described as a *library* rather than a compiler?
2. What problem does a common IR solve, in terms of N frontends and M backends?
3. Name the three isomorphic representations of LLVM IR.
4. In one sentence each: what distinguishes AOT, JIT, and MLIR?
5. What does it mean that "AOT and JIT share the same optimizer and codegen"?
6. Recite the containment hierarchy from `LLVMContext` down to `Instruction`.
7. Why is `Value` the central abstraction?

Next → [02-compilation-pipeline.md](02-compilation-pipeline.md)

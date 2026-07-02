# 05.01 · MLIR — Theory & the Multi-Level Idea

> MLIR (Multi-Level Intermediate Representation) is the most important compiler infrastructure
> idea since LLVM IR itself. It generalizes LLVM's "one IR in the middle" into "**many IRs at
> many abstraction levels, in one framework**." This chapter builds the mental model before we
> touch any code.

---

## 1. The problem MLIR solves

LLVM IR is wonderful, but it lives at *one* level: low, scalar, RISC-like, C-ish. That's too
low for many domains. Consider machine learning: you have tensors, convolutions, and loop
nests. If your only IR is LLVM IR, you must drop *all* that structure immediately:

```
   The "abstraction gap" without MLIR:
   ───────────────────────────────────────────────────────────────────
   matmul(A, B)              ← your domain concept (a single tensor op)
        │
        │  ONE GIANT LEAP straight to...
        ▼
   for i: for j: for k:      ← scalar loops over individual f32 loads/stores
     C[i][j] += A[i][k]*B[k][j]
        │
        ▼
   LLVM IR: load/fmul/fadd/store, br, phi ...

   Problem: once you've lowered to loops/LLVM IR, the optimizer can no longer
   reason "this is a matmul" — it can't fuse two matmuls, tile for cache,
   pick a library kernel, or map to a TPU. The high-level INTENT is gone.
```

Every domain (ML, HPC, hardware design, databases, GPUs) hit the same wall and each built its
*own* ad-hoc IR and infrastructure (TensorFlow had one, XLA another, etc.) — duplicating
parsers, pass managers, SSA, location tracking, verification. MLIR's pitch:

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ Provide ONE infrastructure for building MANY IRs ("dialects") at ANY    │
   │ abstraction level, all sharing SSA, regions, the pass manager, the      │
   │ printer/parser, verification, and lowering machinery. Then optimize     │
   │ at the RIGHT level and lower PROGRESSIVELY toward LLVM IR.              │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. "Multi-level": optimize where it's natural, lower gradually

Instead of one big leap, MLIR inserts a *staircase* of dialects. Each step is a small,
verifiable lowering, and at each level the natural optimizations are easy.

```
   abstraction
     high │  tf / tosa / linalg     "matmul", "conv2d"  ← fuse ops, pick algorithms
          │      │  lower
          │  linalg on tensors      generic loops over tensors ← tiling, fusion
          │      │  lower
          │  affine / scf + memref  explicit loop nests + memory ← loop opts, vectorize
          │      │  lower
          │  arith / vector / cf    scalar & SIMD ops, branches ← classic scalar opts
          │      │  lower
     low  │  llvm dialect           1:1 with LLVM IR ← hand off to LLVM backend
          │      │  translate
          ▼  LLVM IR ──▶ AOT or JIT (sections 03 / 04!)
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ "Progressive lowering": each step lowers ONE level, preserving as much  │
   │ structure as possible for as long as useful. High-level dialects keep   │
   │ domain info for domain optimizations; only near the bottom do you       │
   │ become scalar LLVM IR. Compare ch.00.02's abstraction-level diagram —   │
   │ MLIR fills in the missing middle rungs.                                 │
   └─────────────────────────────────────────────────────────────────────────┘
```

And the bottom of the staircase is the **LLVM dialect**, which maps 1:1 to LLVM IR — so
**MLIR ultimately feeds the AOT and JIT pipelines you already built.** MLIR is not a
replacement for LLVM; it's a tower built *on top of and feeding into* LLVM.

---

## 3. The structural model: everything is an Operation

LLVM IR has a fixed set of instructions. MLIR has *no* fixed instructions — instead a single,
recursive, extensible concept: the **Operation**. Dialects define which operations exist.

```
   An OPERATION (Op) is the universal unit. It has:
   ┌───────────────────────────────────────────────────────────────────────┐
   │  %res:2 = "dialect.opname"(%in1, %in2) {attr = 42} : (T1,T2)->(R1,R2) │
   │   ─────    ──────────────  ───────────  ─────────    ───────────────  │
   │  RESULTS   OP NAME         OPERANDS     ATTRIBUTES    TYPE SIGNATURE  │
   │  (SSA      (dialect-       (SSA values   (compile-     (types of      │
   │   values)   qualified)      it consumes)  time consts)  operands/res) │
   │                                                                       │
   │  ... and optionally REGIONS (nested blocks of ops) ──────────────┐    │
   └──────────────────────────────────────────────────────────────────┼────┘
                                                                      ▼
                                              this is what makes MLIR "nested"
```

The five anatomy parts, each important:

```
  ┌─────────────┬─────────────────────────────────────────────────────────────┐
  │ Results     │ zero or more SSA values the op defines (like LLVM).         │
  │ Operands    │ zero or more SSA values it uses (like LLVM).                │
  │ Attributes  │ COMPILE-TIME constant data (the int 42, a string, an array  │
  │             │ shape). Not SSA values — they're metadata on the op.        │
  │ Types       │ every value is typed; types are also dialect-extensible.    │
  │ Regions     │ THE KEY NEW THING: an op can CONTAIN nested blocks of ops.  │
  │             │ A function body, a loop body, an if-then are regions.       │
  └─────────────┴─────────────────────────────────────────────────────────────┘
```

---

## 4. Regions and blocks — nested SSA

This is the biggest departure from LLVM IR. In LLVM, a function is a flat list of basic
blocks. In MLIR, an op can hold **regions**, each region holds **blocks**, each block holds
**ops** — recursively.

```
   Operation
     └── Region(s)            an op may have 0, 1, or many regions
           └── Block(s)       a region is a list of blocks (a CFG, like LLVM)
                 └── Operation(s)   a block is a list of ops...
                       └── Region(s)   ...which may themselves have regions!
```

```
   Example: an `scf.for` loop op CONTAINS a region (its body):

   %sum = scf.for %i = %c0 to %n step %c1 iter_args(%acc = %init) -> f32 {
            ^bb0(%i: index, %acc: f32):          ◀── a BLOCK inside the region
              %v   = memref.load %A[%i] : memref<?xf32>
              %new = arith.addf %acc, %v : f32
              scf.yield %new : f32               ◀── region terminator
          }
   └──────── the loop body is a nested region, not separate basic blocks ───┘
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ Why nesting matters: it lets high-level ops (loops, conditionals,       │
   │ functions, modules, even whole device kernels) keep their STRUCTURE in  │
   │ the IR. A loop is literally one op with a body region — so a pass can   │
   │ reason about "the loop" as a unit, tile it, fuse it, without first      │
   │ recovering it from a flat CFG (which LLVM passes must do).              │
   └─────────────────────────────────────────────────────────────────────────┘
```

Block arguments replace PHIs: instead of `phi`, a block declares **arguments**, and branches
pass values to them. Same SSA semantics, cleaner model:

```
   LLVM:   %x = phi [a, %bb1], [b, %bb2]
   MLIR:   ^bb3(%x: i32):   ...        ; bb3 takes an argument
           // predecessors: cf.br ^bb3(%a) and cf.br ^bb3(%b)
```

---

## 5. Dialects — namespaced sets of ops/types/attributes

A **dialect** is a self-contained extension: a namespace bundling related operations, types,
and attributes. Dialects are how MLIR is "many IRs."

```
   dialect "arith":   arith.addi, arith.mulf, arith.cmpi, ...      (scalar math)
   dialect "scf":     scf.for, scf.if, scf.while, scf.yield        (structured control flow)
   dialect "func":    func.func, func.call, func.return            (functions)
   dialect "memref":  memref.alloc, memref.load, memref.store      (buffers)
   dialect "linalg":  linalg.matmul, linalg.generic                (tensor algebra)
   dialect "llvm":    llvm.add, llvm.call, llvm.br, ...             (mirrors LLVM IR)
   dialect "gpu","spirv","vector","tensor","tosa","affine", ...    (many more)
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ Crucial property: dialects COEXIST in one module. A function can hold   │
   │ scf + arith + memref ops side by side. Lowering replaces ops of one     │
   │ dialect with ops of a lower one, gradually, until only `llvm` remains.  │
   │ You can also define YOUR OWN dialect for your language/domain (ch.05.04)│
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 6. What MLIR gives you for free

Define a dialect and you immediately inherit the entire shared infrastructure — the thing
each domain used to rebuild:

```
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ • Textual parser & printer (round-trippable .mlir for every dialect).    │
  │ • SSA, dominance, the use-def graph, value/type system.                  │
  │ • A pass manager (analogous to LLVM's, ch.02) that runs on any op.       │
  │ • A generic, declarative rewrite/pattern engine for transforms & lowering│
  │ • Verification: each op declares invariants; the verifier enforces them. │
  │ • Source location tracking threaded through everything.                  │
  │ • Interfaces & traits for writing generic passes across dialects.        │
  │ • TableGen (ODS) to declare ops concisely instead of hand-writing C++.   │
  └──────────────────────────────────────────────────────────────────────────┘
```

This is the real payoff: a new domain IR costs you *op definitions and lowerings*, not a
whole compiler framework.

---

## 7. Where MLIR sits relative to AOT/JIT (the connection)

MLIR does not replace sections 03/04 — it **feeds** them. The bottom of every MLIR pipeline
is "lower to the `llvm` dialect, translate to LLVM IR, then do exactly what you already know."

```
   your language / domain
        │ parse → build a high-level dialect (or use linalg/tosa/...)
        ▼
   MLIR module (high-level dialects)
        │ MLIR passes: domain opts, then progressive lowering (ch.05.03)
        ▼
   MLIR module (llvm dialect only)
        │ translate (ch.05.05): mlir-translate / translateModuleToLLVMIR
        ▼
   LLVM IR  ───────────────────────────────────────────────────────────┐
        │                                                              │
        ├─▶ AOT: TargetMachine → .o → link  (sections 03)              │
        └─▶ JIT: ORC → memory → call        (sections 04)              │
                                          MLIR has its own JIT wrapper
                                          (mlir::ExecutionEngine) that
                                          internally uses ORC for you.
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ Mental model: MLIR is the MISSING UPPER FLOORS of the compiler tower.   │
   │ You climb DOWN through dialects to LLVM IR, then the elevator you built │
   │ in sections 03/04 takes you to native code. Same destination, more      │
   │ floors with better tooling on the way down.                             │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## Mental model checkpoint

1. What is the "abstraction gap" problem with using LLVM IR as your only IR?
2. State MLIR's core pitch in one sentence.
3. What does "progressive lowering" mean, and why keep structure as long as possible?
4. Name the five anatomical parts of an MLIR Operation.
5. What is a region, and how does it make MLIR "nested" vs LLVM's flat CFG?
6. How does MLIR represent what LLVM expresses with PHI nodes?
7. What is a dialect, and what does it mean that dialects "coexist"?
8. List four things you get "for free" by defining a dialect.
9. How does an MLIR pipeline ultimately connect to the AOT/JIT machinery from sections 03/04?

Next → [02-dialects-and-ops.md](02-dialects-and-ops.md)

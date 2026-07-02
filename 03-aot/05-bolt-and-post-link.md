# 03.05 · BOLT, Propeller, PGO & Post-Link Optimization

> The compiler's job *seems* done once the linker writes an executable. But there's a whole
> tier of optimization that happens **at or after link time**, driven by **runtime profiles**,
> that can beat anything `-O3` does alone. The headline tool is **BOLT**. This chapter maps
> the entire post-link / profile-guided landscape: BOLT, Propeller, PGO/AutoFDO/CSSPGO, LTO,
> and Polly — what each is, where it sits in the pipeline, and why it exists.

---

## 1. The core problem: the compiler is flying blind on layout

`-O2`/`-O3` optimize *instructions*. But on modern CPUs, a huge fraction of performance is
lost not to slow instructions but to **bad code layout**: instruction-cache misses, iTLB
misses, and branch mispredictions caused by how functions and basic blocks are *arranged in
memory*.

```
   Where the cycles actually go on a large server binary:
   ┌─────────────────────────────────────────────────────────────────────┐
   │  compute (ALU/FPU) ████████████                                     │
   │  I-cache / iTLB misses ███████████████      ◀── layout problem!     │
   │  branch mispredicts ████████                ◀── layout problem!     │
   │  D-cache misses ██████                                              │
   └─────────────────────────────────────────────────────────────────────┘
   For big apps (databases, browsers, compilers themselves) the front-end
   stalls (fetching instructions) can dominate. Instruction LAYOUT, not
   instruction SELECTION, is the lever.
```

The compiler lays out code before it knows which paths are hot. Even PGO (below) helps only
partially, because the *linker* still shuffles things and inlining decisions are made on
estimates. The fix: optimize the **final binary**, using a profile of how it *actually runs*.

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ KEY IDEA: optimize the program AFTER it's compiled and linked, using    │
   │ a profile of real execution, when you know EXACTLY what's hot and how   │
   │ control flows. This is "post-link optimization."                        │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Where post-link optimization sits in the pipeline

```
   source ─▶ IR ─▶ opt(-O2/-O3) ─▶ codegen ─▶ .o ─▶ LINK ─▶ executable
                       │                                        │
                  (PGO/LTO act                           ┌──────┴────────┐
                   in HERE, at                           │ run with a    │
                   compile/link)                         │ profiler      │
                                                         │ (collect      │
                                                         │  perf data)   │
                                                         └──────┬────────┘
                                                                ▼
                                                       ┌───────────────────┐
                                                       │  BOLT / Propeller │  ◀── POST-LINK
                                                       │  rewrite the      │     (this chapter)
                                                       │  binary using     │
                                                       │  the profile      │
                                                       └────────┬──────────┘
                                                                ▼
                                                       optimized executable
                                                       (re-laid-out, hot code
                                                        packed together)
```

Note post-link optimizers operate on **machine code in the linked binary**, not on LLVM IR.
They are *binary-to-binary* transformers. That's what makes them different from everything
else in this guide.

---

## 3. BOLT — Binary Optimization and Layout Tool

**BOLT** (originally from Meta, now part of the LLVM project under `bolt/`) is a post-link
optimizer. You give it a linked executable plus a profile; it emits a faster executable with
the same behavior.

### The BOLT workflow

```
   STEP 1: build normally (ideally WITH relocations kept: link with --emit-relocs)
       cc -O2 ... -Wl,--emit-relocs -o app

   STEP 2: profile the running binary
       (a) perf:        perf record -e cycles:u -j any,u -o perf.data -- ./app <workload>
                        perf2bolt -p perf.data -o app.fdata app
       (b) or BOLT's own instrumentation:
                        llvm-bolt app -instrument -o app.inst
                        ./app.inst <workload>     # writes a .fdata profile

   STEP 3: optimize with the profile
       llvm-bolt app -o app.bolt -data=app.fdata \
                 -reorder-blocks=ext-tsp -reorder-functions=hfsort \
                 -split-functions -icf=1 -dyno-stats

   STEP 4: ship app.bolt  (typically 2–15% faster on large apps; sometimes more)
```

```
   ┌──────────────────────────────── BOLT ──────────────────────────────────┐
   │                                                                        │
   │   app (ELF)  ──▶  disassemble & rebuild a CFG for every function       │
   │   app.fdata  ──▶  annotate CFG edges with execution counts             │
   │                        │                                               │
   │                        ▼                                               │
   │              run a suite of binary-level passes (see below)            │
   │                        │                                               │
   │                        ▼                                               │
   │   re-emit code with a NEW layout  ──▶  app.bolt (faster ELF)           │
   └────────────────────────────────────────────────────────────────────────┘
```

### What BOLT actually does (its passes)

```
  ┌─────────────────────┬────────────────────────────────────────────────────┐
  │ Basic-block reorder │ place hot blocks contiguously, push cold/error     │
  │ (ext-tsp)           │ paths away → fewer taken branches, better I-cache. │
  │                     │ "ext-tsp" = extended Traveling-Salesman layout.    │
  ├─────────────────────┼────────────────────────────────────────────────────┤
  │ Function reordering │ cluster hot functions together (hfsort/hfsort+) so │
  │ (hfsort)            │ functions that call each other share cache/TLB.    │
  ├─────────────────────┼────────────────────────────────────────────────────┤
  │ Function splitting  │ split each function into hot + cold fragments;     │
  │                     │ move cold fragments to a separate region so the    │
  │                     │ hot core is dense (less I-cache pollution).        │
  ├─────────────────────┼────────────────────────────────────────────────────┤
  │ ICF                 │ Identical Code Folding: merge functions with       │
  │                     │ identical machine code into one.                   │
  ├─────────────────────┼────────────────────────────────────────────────────┤
  │ Indirect-call promo │ turn hot indirect/virtual calls into a checked     │
  │                     │ direct call (speculative devirtualization).        │
  ├─────────────────────┼────────────────────────────────────────────────────┤
  │ PLT optimization,   │ inline PLT stubs, fix up alignment for branch      │
  │ alignment, etc.     │ predictor & fetch efficiency.                      │
  └─────────────────────┴────────────────────────────────────────────────────┘
```

The single biggest win is usually **block + function reordering**:

```
   BEFORE (compiler order: hot & cold interleaved)
   ┌────────────────────────────────────────────────────────────┐
   │ funcA_hot | funcA_cold | funcB_cold | funcB_hot | funcC... │   ← I-cache lines
   └────────────────────────────────────────────────────────────┘
     cache pulls in cold bytes alongside hot ones → wasted cache, more misses

   AFTER BOLT (hot packed, cold exiled)
   ┌─────────────────────────────┐      ┌─────────────────────────────┐
   │ funcA_hot funcB_hot funcC.. │ .... │ funcA_cold funcB_cold ...   │
   └─────────────────────────────┘      └─────────────────────────────┘
        HOT region (dense, cache-friendly)     COLD region (rarely touched)
```

```
   Inside one function, block reordering makes the common path fall through:

   BEFORE                         AFTER ext-tsp
   ───────────────────            ───────────────────
   entry:                         entry:
     test; jne cold   ──┐           test; je  cold      ; flip: rare path branches
     <hot body>         │           <hot body>          ; hot path is straight-line
     ret                │           ret                 ; (fall-through, predicted)
   cold:  <rare> ◀──────┘         cold:  <rare>         ; moved out of the hot stream
```

### Why BOLT beats compiler PGO at layout

```
   ┌────────────────────────────────────────────────────────────────────────┐
   │ • The compiler optimizes per-function, before linking & inlining are   │
   │   final. BOLT sees the WHOLE linked binary with FINAL addresses.       │
   │ • Profiles from `perf` on the real binary are precise (no IR-to-binary │
   │   mapping drift that degrades source-level PGO).                       │
   │ • BOLT can move things the linker already placed — it's the last word  │
   │   on layout. PGO + LTO + BOLT STACK: each adds gains the others can't. │
   └────────────────────────────────────────────────────────────────────────┘
```

Real-world: BOLT is used on Meta's services, the Linux kernel, databases, and — famously —
**Clang/LLVM itself** (a BOLTed clang compiles faster). It's complementary to PGO/LTO, not a
replacement: the recommended recipe is `ThinLTO + PGO + BOLT`.

---

## 4. Propeller — post-link layout, the scalable way

BOLT rewrites the whole binary in one shot, which is memory-hungry for very large programs.
**Propeller** (also LLVM) achieves similar layout gains but integrates with the **compiler
and linker** instead of rewriting the final binary monolithically.

```
   BOLT model:                          Propeller model:
   ───────────────────────              ──────────────────────────────────
   linked exe + profile                 1. compile with -fbasic-block-sections
       │  monolithic rewrite               (each basic block in its OWN section)
       ▼                                 2. profile, produce a layout plan
   optimized exe                         3. RE-LINK with lld using the plan
                                            (lld places sections per the profile)
                                         → layout decisions deferred to the
                                           linker; distributable & scalable
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ Propeller = "do BOLT-style layout, but express it as basic-block        │
   │ SECTIONS that the LINKER reorders," so it scales to huge binaries and   │
   │ fits a distributed build. Trade-off: needs recompilation + relink;      │
   │ BOLT needs only the final binary + profile.                             │
   └─────────────────────────────────────────────────────────────────────────┘
```

Use BOLT when you only have/want to touch the final binary; use Propeller when you control
the build and need it to scale.

---

## 5. The profile family: PGO, AutoFDO, CSSPGO

Post-link tools need profiles. So does compile-time **PGO**. Know the variants — they differ
in *how the profile is collected* and *how precise it is*.

```
  ┌───────────────┬────────────────────────────────────────────────────────────┐
  │ Instrumented  │ Compiler inserts counters (-fprofile-generate). Run the    │
  │ PGO           │ app → exact counts. Most accurate, but instrumented build  │
  │               │ is slow (~2x) and needs a separate profiling build.        │
  ├───────────────┼────────────────────────────────────────────────────────────┤
  │ Sampling PGO  │ Profile a NORMAL optimized build with `perf` (hardware     │
  │ / AutoFDO     │ sampling). No instrumentation, low overhead (~1%), can     │
  │               │ profile in production. Less precise; needs debug info to   │
  │               │ map samples back to source/IR.                             │
  ├───────────────┼────────────────────────────────────────────────────────────┤
  │ CSSPGO        │ Context-Sensitive Sampling PGO: AutoFDO + pseudo-call-stack│
  │               │ context (via synthetic probes) so inlining/specialization  │
  │               │ decisions get per-call-site profiles. More accurate FDO.   │
  ├───────────────┼────────────────────────────────────────────────────────────┤
  │ Temporal /    │ Profiles that capture ORDER of execution (e.g. startup vs  │
  │ partial PGO   │ steady-state) to optimize phases differently.              │
  └───────────────┴────────────────────────────────────────────────────────────┘
```

How profiles flow into the two consumers:

```
                ┌──────── collect profile ─────────┐
                │                                  │
        instrumented run                    perf sampling
        (-fprofile-generate)                (AutoFDO/CSSPGO)
                │                                  │
                ▼                                  ▼
        .profdata ──▶ COMPILE-TIME PGO        perf.data ──▶ perf2bolt
        (clang -fprofile-use):                ──▶ .fdata ──▶ POST-LINK (BOLT)
          better inlining, block              layout of the FINAL binary
          ordering, branch hints,
          register allocation
```

The punchline: **the same idea (profile-guided) is applied at two stages.** Compile-time PGO
shapes *optimization and codegen*; post-link BOLT shapes *final layout*. They stack.

---

## 6. LTO recap, and how it composes with PGO + BOLT

LTO (covered in [../02-ir-and-passes/03-pass-pipeline.md](../02-ir-and-passes/03-pass-pipeline.md) §7)
optimizes across translation units at link time. It's orthogonal to profiling, and the
full-strength production recipe combines all three:

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │  THE "MAX PERFORMANCE" AOT STACK (each layer adds non-overlapping wins):│
   │                                                                         │
   │   1. ThinLTO       cross-module inlining & whole-program opt            │
   │        +                                                                │
   │   2. PGO/AutoFDO    profile-guided inlining, block order, branch hints  │
   │        +                                                                │
   │   3. BOLT/Propeller post-link layout of the final binary                │
   │                                                                         │
   │   Typical compounding: each can add a few %; together, double digits    │
   │   on front-end-bound workloads. Order: LTO+PGO at build, BOLT after.    │
   └─────────────────────────────────────────────────────────────────────────┘
```

```
   build ──[ThinLTO + PGO]──▶ fast binary ──[profile + BOLT]──▶ faster binary
   └──── compiler/linker tier ────┘         └──── post-link tier ────┘
```

---

## 7. Polly — polyhedral loop optimization (a different "other stuff")

Not post-link, but worth knowing as another LLVM optimization subsystem you'll hear about.
**Polly** is a high-level loop optimizer built on the *polyhedral model*: it represents loop
nests as mathematical polyhedra and applies transformations (tiling, fusion, interchange,
parallelization, vectorization-enabling reshaping) that classic pass-by-pass optimizers
struggle to do.

```
   classic LLVM loop opts: pattern-based, one loop at a time.
   Polly: model the entire loop nest's iteration space as polyhedra, then
          use integer linear programming to find a better schedule.

   for i: for j: A[i][j] = A[i][j] + B[j][i]
       │  Polly sees the iteration space {(i,j) | 0<=i<N, 0<=j<M}
       ▼  and can TILE / INTERCHANGE / PARALLELIZE for cache & SIMD
   tiled, cache-blocked, optionally OpenMP-parallel loop nest
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ Polly is to LOOPS what BOLT is to LAYOUT: a specialized, heavier        │
   │ machine that finds optimizations the default pipeline can't express.    │
   │ It plugs into the pass pipeline (-O3 -mllvm -polly). Big wins on dense  │
   │ numerical / array code; niche for general code.                         │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 8. The whole landscape on one map

```
   ABSTRACTION         TOOL/TECH              WHAT IT OPTIMIZES        WHEN
   ───────────         ────────────           ──────────────────      ───────────
   loop nests          Polly                  loop schedule/tiling    compile time
   whole program IR    LTO / ThinLTO          cross-module inlining   link time
   IR (profile-aware)  PGO/AutoFDO/CSSPGO     inlining, branch, regs  compile time
   machine code        BOLT                   layout of final binary  POST-link
   machine code        Propeller              layout via BB sections  link (+recompile)
   machine code        -O2/-O3 backend        instruction selection,  compile time
                                              scheduling, regalloc
```

```
   "When is each optimization possible?" — the recurring theme of this guide:
   the LATER you optimize, the MORE you know (final addresses, real profiles),
   but the LESS you can change (you're editing bytes, not IR). BOLT trades
   flexibility for perfect information; Polly/LTO trade information for power.
```

---

## 9. Practical: when to reach for what

```
  ┌───────────────────────────────────────┬────────────────────────────────────┐
  │ Situatio n                            │ Reach for                          │
  ├───────────────────────────────────────┼────────────────────────────────────┤
  │ Large, front-end-bound service        │ ThinLTO + PGO, then BOLT           │
  │ (DB, browser, compiler)               │                                    │
  │ Can't recompile, only have the binary │ BOLT (instrumentation mode)        │
  │ Huge binary, distributed build        │ Propeller                          │
  │ Dense numerical / array kernels       │ Polly (or move to MLIR — sec. 05)  │
  │ Can't profile in prod cheaply         │ AutoFDO/CSSPGO (sampling)          │
  │ Multi-TU C++ with lots of inlining    │ ThinLTO                            │
  │ Just want a fast single binary, easy  │ -O2/-O3 (you already have it)      │
  └───────────────────────────────────────┴────────────────────────────────────┘
```

> These post-link/profile tools are an **AOT-only** luxury: they require a fixed binary and a
> representative workload to profile. A JIT gets analogous benefits *for free and
> continuously* by observing real execution and reoptimizing hot code live — which is exactly
> the subject of the next section. (See [../04-jit/04-advanced-jit.md](../04-jit/04-advanced-jit.md)
> on tiered/profile-guided reoptimization — the JIT analog of PGO+BOLT.)

---

## Mental model checkpoint

1. Why can code *layout* dominate performance even when instruction selection is optimal?
2. What inputs does BOLT take, and what does it produce?
3. Name three BOLT passes and what each improves.
4. Why can BOLT beat compile-time PGO at layout specifically?
5. How does Propeller differ from BOLT in mechanism and scalability?
6. Distinguish instrumented PGO, AutoFDO, and CSSPGO by collection method and precision.
7. State the "max performance AOT stack" and the order you'd apply its layers.
8. What does Polly do that the default loop passes don't, and what's its model?
9. Restate the guide's recurring theme: how does *when* you optimize trade information for
   flexibility, using BOLT and LTO as the extremes?

Next → [../04-jit/01-jit-theory.md](../04-jit/01-jit-theory.md)

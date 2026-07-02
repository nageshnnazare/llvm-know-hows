# 04.04 · Advanced JIT — Lazy, Tiered, Speculative, Remote

> The basic JIT compiles everything eagerly at `-O2`. Real-world JITs (JVM, V8, Julia) do
> much more: compile lazily, recompile hot code at higher tiers, speculate on runtime facts
> and deoptimize when wrong, and even run code in another process. This chapter explains
> these techniques and how ORC supports them.

---

## 1. Lazy compilation — compile on first call

Why compile a function that's never called? **Lazy compilation** defers a function's codegen
until it's actually invoked, via a **stub** that triggers compilation on first entry.

```
   At "add" time, install a tiny STUB for fib instead of compiling it:

   fib_stub:  jmp *fib_ptr      ; fib_ptr initially → the "compile me" trampoline

   First call to fib:
     → trampoline runs: ORC materializes fib (transform→compile→link)
     → fib_ptr updated to point at the real compiled fib
     → re-dispatch into real fib
   Second call onward:
     → fib_stub jumps straight to compiled fib (one extra indirect jump, then
        often patched away)
```

```
   ┌────────────────────────────────────────────────────────────────────────┐
   │ Lazy compilation = pay codegen cost only for code that actually runs.  │
   │ Huge for startup of large programs where most functions never execute. │
   │ ORC implements this with lazy reexports / indirect stubs + the         │
   │ on-request materialization core (chapter 04.02 §2).                    │
   └────────────────────────────────────────────────────────────────────────┘
```

In ORC, this is the difference between eager `LLJIT` and `LLLazyJIT`, or building lazy
reexports yourself with `lazyReexports` and an indirect-stubs manager.

```cpp
// Conceptual: create lazy reexports so symbols compile on first lookup/call.
// LLLazyJIT wires the CompileOnDemandLayer + lazy call-through manager for you.
auto J = cantFail(LLLazyJITBuilder().create());
// addIRModule as usual; functions now compile the first time they're called.
```

---

## 2. The CompileOnDemandLayer and per-function laziness

ORC's `CompileOnDemandLayer` splits a module so each function can be materialized
independently, on demand.

```
   addIRModule(M with f, g, h)
        │  CompileOnDemandLayer splits into per-function materialization units
        ▼
   JITDylib has stubs for f, g, h  (none compiled yet)
        │
   call g() ─▶ only g compiled & linked; f, h still stubs
        │
   call f() ─▶ now f compiled; h still a stub forever if never called
```

```
   Granularity ladder:
     LLJIT            : whole module compiled on first symbol lookup
     LLLazyJIT / COD  : each FUNCTION compiled on first call
     (custom)         : even finer, e.g. compile then re-split for tiering
```

---

## 3. Tiered compilation — the heart of fast JITs

The compile-vs-run tension (chapter 04.01 §3) is resolved by **tiers**: start with cheap,
fast-to-produce code; promote hot functions to expensive, fast-to-run code.

```
   TIER 0: interpreter / template JIT     (instant, slow execution)
       │   counts calls/loop iterations (profiling counters)
       │   gets hot →
   TIER 1: quick JIT (-O0/-O1, fast compile, decent code)
       │   still hot, stays hot →
   TIER 2: optimizing JIT (-O2/-O3, slow compile, fast code)
       │   uses profile data gathered in lower tiers!
       ▼
   steady state: hottest code runs at top tier; cold code never leaves tier 0/1
```

```
   ┌────────────────────────────────────────────────────────────────────────┐
   │ Tiered = "spend compile time in proportion to how hot the code is."    │
   │ Cold code: cheap. Hot code: lavish optimization + the runtime profile  │
   │ gathered while it ran in lower tiers. This is the JIT analog of        │
   │ AOT's PGO+BOLT (chapter 03.05) — but automatic and continuous.         │
   └────────────────────────────────────────────────────────────────────────┘
```

The mechanism in ORC: compile a baseline version, install counting/profiling, and when a
threshold trips, **re-materialize** the function at a higher optimization level and **hot-swap
the pointer** (via the same indirection used for lazy stubs).

```
   fib_ptr ──▶ baseline fib (tier 1)     [call counter ticks]
                    │ counter > threshold
                    ▼  background thread compiles -O3 fib
   fib_ptr ──▶ optimized fib (tier 2)    [future calls go here]
   (the indirection slot makes the swap atomic & transparent to callers)
```

---

## 4. Speculative optimization & deoptimization

The reason JITs can *beat* AOT: they bet on runtime observations and keep an escape hatch.

```
   Observation: "x has been an integer in 100% of 50,000 executions."
   Speculation: compile a fast path assuming x is an integer.
   Guard:       if (!is_int(x)) goto deopt;       ◀ cheap check
   Fast path:   <optimized integer code, inlined, no boxing>
   Deopt:       reconstruct interpreter state, fall back to generic code,
                maybe discard & recompile without this assumption.
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ DEOPTIMIZATION = the ability to abandon optimized code mid-execution    │
   │ and resume in a safe, generic version, restoring the exact program      │
   │ state. It's what makes speculation SOUND: if the bet is wrong, you      │
   │ don't get wrong answers, you just get slower (this time).               │
   └─────────────────────────────────────────────────────────────────────────┘
```

LLVM support: **stack maps** and the `@llvm.experimental.stackmap` /
`@llvm.experimental.patchpoint` intrinsics let the JIT record where values live (registers,
stack slots) at guard points, so a deopt handler can reconstruct state. Patchpoints reserve
space to later overwrite a call site (e.g., to install a guard or swap a target).

```
   patchpoint: emit a NOP sled of N bytes at a call site + a stackmap entry.
       │  later, the runtime can patch those bytes (e.g. redirect to deopt or
       │  to a newly-specialized callee) knowing the live-value locations.
       ▼  enables inline caches, guard insertion, on-stack replacement (OSR).
```

This is advanced and language-runtime-specific; LLVM provides the primitives, the runtime
provides the policy.

---

## 5. On-Stack Replacement (OSR)

A special, important case: a function is running a long loop in tier-1 code when it gets hot.
You can't wait for it to return to swap in optimized code — so you replace it **while it's on
the stack, mid-loop**.

```
   tier-1 frame running:  for (i=0; i<1e9; i++) { ...hot... }   ◀ stuck here, hot
        │  OSR: at a loop back-edge safepoint,
        │  capture live state (i, accumulators) via stackmap,
        │  build/enter the optimized version SEEDED with that state,
        ▼  continue the loop in tier-2 code from iteration i
   tier-2 frame running:  ...optimized loop continues from i...
```

OSR is why a JIT can speed up a long-running loop that was entered before it was known to be
hot. It relies on the same stackmap machinery as deopt.

---

## 6. Remote / out-of-process JIT (ORC TPC & ORC-RT)

From chapter 04.02 §5: ORC can compile in one process and execute in another via
`ExecutorProcessControl` and the ORC runtime (`orc-rt`).

```
   ┌──────────────── controller process ──────────────────┐   ┌──── executor ────┐
   │ frontend → IR → optimize → compile (TargetMachine    │   │ receives objects │
   │ for the EXECUTOR's triple) → object bytes            │──▶│ allocates memory │
   │ ExecutionSession drives lookup via EPC over a channel│   │ links, relocates │
   │ (pipe/socket/shared mem)                             │◀──│ runs the code    │
   └──────────────────────────────────────────────────────┘   └──────────────────┘
```

```
   Use cases:
     • SECURITY: JIT'd (possibly untrusted) code runs sandboxed; a crash or
       exploit can't corrupt the compiler/host.
     • CROSS-TARGET: compile on a beefy x86 host, execute on a tiny ARM board.
     • RESOURCE ISOLATION: separate the compiler's memory from the program's.
   EPC abstracts memory allocation, symbol lookup, and "run this" across the
   boundary so your JIT code looks almost identical to the in-process case.
```

---

## 7. Choosing a JIT design

```
  ┌─────────────────────────────────────────┬────────────────────────────────────┐
  │ Need                                    │ Design                             │
  ├─────────────────────────────────────────┼────────────────────────────────────┤
  │ Embed codegen, run a few functions      │ LLJIT, eager, -O2                  │
  │ REPL / eval / scripting                 │ LLJIT + ResourceTrackers           │
  │ Large program, fast startup             │ LLLazyJIT (lazy per-function)      │
  │ Long-running server, max steady-state   │ tiered: baseline + background -O3  │
  │ Dynamic language (types observed)       │ speculation + deopt + stackmaps    │
  │ Long hot loops entered "cold"           │ + OSR                              │
  │ Untrusted code / cross-target           │ out-of-process (EPC + orc-rt)      │
  └─────────────────────────────────────────┴────────────────────────────────────┘
```

```
   The progression of this chapter mirrors how real JITs evolved:
     eager  →  lazy  →  tiered  →  speculative+deopt  →  OSR  →  remote
   Each step trades simplicity for either lower latency or higher peak speed.
   You rarely need them all; pick the smallest design that meets your goal.
```

---

## 8. The grand comparison: AOT vs JIT, settled

```
  ┌────────────────────┬───────────────────────────┬───────────────────────────┐
  │ Dimension          │ AOT (sections 03)         │ JIT (sections 04)         │
  ├────────────────────┼───────────────────────────┼───────────────────────────┤
  │ When compiled      │ before run, offline       │ during run, in-process    │
  │ Startup            │ instant                   │ warmup cost               │
  │ Peak performance   │ high (static info only)   │ potentially higher        │
  │                    │                           │ (runtime info, specialize)│
  │ Uses profiles via  │ PGO/AutoFDO + BOLT (man.) │ tiering + deopt (auto)    │
  │ Ships              │ native binary             │ source/IR + a compiler    │
  │ Memory at runtime  │ just the program          │ program + compiler        │
  │ Layout tuning      │ BOLT/Propeller post-link  │ continuous re-layout      │
  │ Restricted envs    │ works (no runtime codegen)│ may be forbidden (W^X)    │
  │ Reuses LLVM        │ optimizer + backend       │ SAME optimizer + backend  │
  │                    │ + write .o + link         │ + ORC in-memory link      │
  └────────────────────┴───────────────────────────┴───────────────────────────┘
```

The deepest point of this whole guide:

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ AOT and JIT are the SAME compiler with the delivery step swapped. You   │
   │ build ONE frontend, reuse ONE optimizer and ONE backend, and choose at  │
   │ the end whether the bytes go to a FILE (AOT) or to MEMORY you call into │
   │ (JIT). Everything else — tiering, BOLT, deopt — is policy on top.       │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## Mental model checkpoint

1. How does a stub enable lazy compilation, and what gets patched on first call?
2. What does `CompileOnDemandLayer` give you over plain `LLJIT`?
3. Explain tiered compilation and why hot code benefits twice (compile budget + profile).
4. What is deoptimization and why does it make speculation *sound*?
5. What LLVM primitives (intrinsics/maps) support deopt and call-site patching?
6. What problem does OSR solve that ordinary tier promotion can't?
7. Give two reasons to run a JIT out-of-process.
8. State the one-sentence relationship between an AOT and a JIT compiler.

JIT complete. Next, MLIR → [../05-mlir/01-mlir-theory.md](../05-mlir/01-mlir-theory.md)

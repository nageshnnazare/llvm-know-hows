# 02.03 · The Optimization Pipeline — How `-O2` Is Built

> Individual passes are Lego bricks. The *pipeline* is the model. This chapter shows how
> LLVM assembles its `-O0..-O3` pipelines, the phase ordering logic, and how to inspect,
> customize, and reason about pass ordering — knowledge you need to tune both AOT and JIT.

---

## 1. Why ordering matters (the phase-ordering problem)

Passes enable each other. Run them in the wrong order and you leave optimizations on the
table. A classic chain:

```
   mem2reg          turns memory into SSA registers
      │             ── now values are visible to...
      ▼
   instcombine      simplifies expressions
      │             ── exposes constants for...
      ▼
   sccp / gvn       constant prop + redundancy elimination
      │             ── creates dead code & trivial branches for...
      ▼
   simplifycfg      cleans up the CFG
      │             ── may expose MORE simplifications, so we LOOP back
      ▼
   (repeat key passes; inline first to expose cross-call opportunities)
```

```
   ┌────────────────────────────────────────────────────────────────────────┐
   │ KEY INSIGHT: optimization is not a single forward pass. Pipelines      │
   │ INTERLEAVE and REPEAT passes because each creates opportunities for    │
   │ others. Inlining especially is run early so everything after it sees   │
   │ the combined code. This is "phase ordering," an open research problem. │
   └────────────────────────────────────────────────────────────────────────┘
```

---

## 2. The shape of the default `-O2` pipeline

`PassBuilder::buildPerModuleDefaultPipeline(O2)` produces (heavily abbreviated) something
like:

```
   Module pipeline
   ├── early cleanup (always-inline, coro, etc.)
   ├── GLOBAL phase
   │     ├── ipsccp            interprocedural constant prop
   │     ├── globalopt         optimize globals
   │     └── ...
   ├── CGSCC phase (bottom-up over the call graph)
   │     ├── inliner            ◀── the big one; inlines hot/small callees
   │     └── function simplification pipeline, per function:
   │           ├── sroa / mem2reg
   │           ├── early-cse
   │           ├── instcombine
   │           ├── simplifycfg
   │           ├── reassociate
   │           ├── loop pipeline: licm, indvars, unroll, ...
   │           ├── gvn
   │           ├── sccp
   │           ├── instcombine (again!)
   │           └── simplifycfg (again!)
   ├── LOOP/VECTORIZE phase
   │     ├── loop-vectorize
   │     ├── slp-vectorize
   │     └── instcombine + simplifycfg cleanup
   └── late module cleanup (globaldce, constmerge, ...)
```

```
       source IR
          │
   [ interprocedural ] ── ipsccp, globalopt
          │
   [ inline + per-fn  ] ── the "function simplification pipeline", run after inlining
   [ simplification   ]    so callee bodies get optimized in caller context
          │
   [ loop opts        ] ── licm, unroll, vectorize
          │
   [ vectorize        ]
          │
   [ cleanup          ] ── globaldce
          │
       optimized IR ──▶ backend
```

Notice `instcombine` and `simplifycfg` appear **multiple times**. That's intentional —
they're cheap "cleanup" passes run after anything that creates mess.

---

## 3. `-O0` to `-O3`, `-Os`, `-Oz`: what changes

```
  ┌───────┬─────────────────────────────────────────────────────────────────┐
  │ -O0   │ almost nothing. (Note: even -O0 runs mem2reg-like cleanup only  │
  │       │ if you ask; clang -O0 keeps allocas → debuggable but slow.)     │
  │ -O1   │ light: basic simplification, no aggressive inlining/vectorize.  │
  │ -O2   │ the standard "fast" build: inlining, GVN, loop opts, vectorize. │
  │ -O3   │ -O2 + more aggressive inlining & vectorization (bigger code).   │
  │ -Os   │ optimize but bias toward SMALL code size.                       │
  │ -Oz   │ minimize size even harder (e.g. avoid unrolling/vectorizing).   │
  └───────┴─────────────────────────────────────────────────────────────────┘
```

The same `PassBuilder` builds all of them; the `OptimizationLevel` tweaks thresholds (inline
cost, unroll factor) and toggles a few passes. There's no separate hand-written pipeline per
level — it's parameterized.

---

## 4. Inspecting and debugging pipelines

These `opt` flags are how you *learn* the pipeline and debug pass interactions:

```bash
# Print the exact pass structure for a given -O level:
>>> opt -passes='default<O2>' -print-pipeline-passes -S in.ll -o /dev/null

# Dump IR after every pass (firehose — pipe to a pager / file):
>>> opt -passes='default<O2>' -print-after-all -S in.ll -o /dev/null 2> trace.ll

# Dump IR only after a specific pass:
>>> opt -passes='default<O2>' -print-after=gvn -S in.ll -o /dev/null

# See which passes changed the IR (skip no-op passes):
>>> opt -passes='default<O2>' -print-changed -S in.ll -o /dev/null

# Time each pass (find the expensive ones):
>>> opt -passes='default<O2>' -time-passes -S in.ll -o /dev/null
```

```
   -print-after-all is your microscope: it shows the IR mutate pass by pass,
   so you can SEE mem2reg create PHIs, instcombine fold expressions, etc.
   Run it once on a small function — it teaches the pipeline better than any docs.
```

---

## 5. Constructing pipelines in code (the API recap)

From [02-optimization-passes.md](02-optimization-passes.md), the three ways:

```cpp
// (a) Standard pipeline by level:
ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);

// (b) Parse a textual pipeline (mirrors opt -passes=...):
ModulePassManager MPM;
cantFail(PB.parsePassPipeline(MPM, "default<O2>"));
// or a custom one:
cantFail(PB.parsePassPipeline(MPM, "function(mem2reg,instcombine,gvn,simplifycfg)"));

// (c) Hand-assemble:
FunctionPassManager FPM;
FPM.addPass(PromotePass());
FPM.addPass(InstCombinePass());
ModulePassManager MPM2;
MPM2.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
```

### Hooking custom passes into a standard pipeline

You can inject your pass at well-defined "extension points" without rebuilding the whole
pipeline — e.g., run your pass right after the simplification pipeline:

```cpp
PB.registerScalarOptimizerLateEPCallback(
    [](FunctionPassManager &FPM, OptimizationLevel) {
      FPM.addPass(MyCustomPass());
    });
ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
```

```
   Extension points (EP callbacks) let you say "run my pass HERE in the
   standard flow" rather than reconstructing the pipeline:
     PipelineStartEP            — very beginning
     PeepholeEP                 — alongside instcombine cleanup spots
     ScalarOptimizerLateEP      — after scalar simplification
     OptimizerLastEP            — at the very end (module level)
     VectorizerStartEP, ...     — around vectorization
```

---

## 6. Pipelines for AOT vs JIT vs MLIR — practical differences

The *pipeline* differs by use case even though the *passes* are the same:

```
  ┌───────────┬─────────────────────────────────────────────────────────────┐
  │ AOT       │ usually -O2/-O3 full pipeline. Compile time is paid once,   │
  │           │ offline, so spend it freely. Often + LTO across modules.    │
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ JIT       │ tension: every ms of compile time delays execution. Choices:│
  │           │  • baseline: a CHEAP pipeline (mem2reg + a few) for fast    │
  │           │    startup, then RE-OPTIMIZE hot functions at -O2/-O3 in    │
  │           │    the background ("tiered compilation").                   │
  │           │  • or just -O2 if latency-tolerant.                         │
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ MLIR      │ TWO pipelines: high-level MLIR passes (on dialects) BEFORE  │
  │           │ lowering, then the standard LLVM pipeline AFTER reaching    │
  │           │ LLVM IR. Domain opts happen up high; classic opts down low. │
  └───────────┴─────────────────────────────────────────────────────────────┘
```

Tiered JIT compilation, visualized (foreshadowing [../04-jit/04-advanced-jit.md](../04-jit/04-advanced-jit.md)):

```
   call site cold ──▶ compile FAST (cheap pipeline) ──▶ run, count calls
                                                          │ gets hot
                                                          ▼
                               recompile at -O3 in background, hot-swap pointer
```

---

## 7. LTO: optimizing across translation units (AOT-specific)

Normally each module is optimized in isolation. **Link-Time Optimization** defers some
optimization to link time, when the *whole program* is visible, enabling cross-module
inlining and global analysis.

```
   Normal:  a.c→a.o (opt'd alone)   b.c→b.o (opt'd alone)   ── link ──▶ exe
            cross-module inlining impossible (only .o bytes at link).

   LTO:     a.c→a.bc (IR!)          b.c→b.bc (IR!)
                       ╲           ╱
                        merge IR, optimize WHOLE program, THEN codegen ──▶ exe
            cross-module inlining, global DCE, devirtualization possible.
```

- **Full LTO:** merge all IR into one giant module, optimize, codegen. Best results, high
  memory/time.
- **ThinLTO:** keep modules separate but exchange summaries for cross-module inlining
  decisions; parallel and scalable. The default "LTO" most projects use.

Driven by `clang -flto` / `-flto=thin`; in your own AOT driver you'd emit bitcode and let the
linker (with the LLVM gold/lld plugin) run the LTO pipeline. JITs don't typically use LTO
(they compile incrementally).

---

## Mental model checkpoint

1. Why is optimization not a single forward pass over the IR?
2. Why do `instcombine` and `simplifycfg` appear multiple times in `-O2`?
3. Why is the inliner run before the per-function simplification pipeline?
4. What distinguishes `-O2`, `-O3`, `-Os`, `-Oz` — different pipelines or different params?
5. Which `opt` flag lets you watch the IR change after each pass?
6. How does a JIT resolve the compile-time-vs-quality tension?
7. What does LTO enable that per-module optimization cannot, and how does ThinLTO scale it?

IR & passes complete. Next, AOT compilation → [../03-aot/01-aot-theory.md](../03-aot/01-aot-theory.md)

# 04.01 · JIT Compilation — Theory

> Just-In-Time compilation generates native machine code **at runtime, inside the running
> process**, and jumps to it immediately. This chapter builds the mental model: what JIT is,
> why it exists, the in-process execution model, and how it relates to (and reuses) the AOT
> machinery you just learned.

---

## 1. The defining characteristic

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │  JIT: compilation happens DURING execution, in the SAME process, and    │
   │       the freshly-generated machine code is run IMMEDIATELY via a       │
   │       function pointer. No file on disk, no separate run step.          │
   └─────────────────────────────────────────────────────────────────────────┘
```

Contrast the timelines with AOT:

```
   AOT:  [compile ............] then later, elsewhere: [run]
         two processes, two times, two (maybe) machines.

   JIT:  [............ one process ............]
         [ run a bit ][ compile fn X ][ run X ][ compile fn Y ][ run Y ]...
         compilation and execution INTERLEAVE in the same address space.
```

The signature move of a JIT:

```cpp
   // 1. produce machine code for `f` into executable memory at address P
   // 2. cast that address to a function pointer
   auto fp = (double(*)(double)) P;
   // 3. CALL IT — right now, in this process
   double result = fp(3.0);
```

That `fp(3.0)` jumps into code that **did not exist a millisecond ago**. That's the whole
idea.

---

## 2. Why JIT? The runtime-information advantage

AOT must decide everything statically. A JIT runs *while the program runs*, so it can use
**facts only knowable at runtime**:

```
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ • Actual hot paths: profile execution, then optimize only what's hot.    │
  │ • Observed types: in dynamic languages, "this variable has been an int   │
  │   10000 times" → specialize/inline for int, guard for the rest.          │
  │ • Real constant values: a config read at startup becomes a compile-time  │
  │   constant for code generated afterward.                                 │
  │ • The exact host CPU: emit AVX-512 because we KNOW this machine has it.  │
  │ • Late binding / eval: compile code that didn't exist at build time      │
  │   (REPLs, shaders, generated queries, user scripts).                     │
  └──────────────────────────────────────────────────────────────────────────┘
```

```
   This is "speculative optimization": assume the common case observed at
   runtime, compile a fast path for it, and keep a guard + fallback (deopt)
   for when the assumption breaks. AOT literally cannot do this — it has no
   runtime to observe. It's the JIT's superpower.
```

---

## 3. The costs and the tension

JIT isn't free; the compiler runs *during* your program's execution:

```
  ┌─────────────────────────────────────┬────────────────────────────────────────┐
  │ COSTS                               │ MITIGATIONS                            │
  ├─────────────────────────────────────┼────────────────────────────────────────┤
  │ • Startup/warmup latency: first     │ • Tiered compilation: cheap/fast code  │
  │   calls are slow (compile then run).│   first, reoptimize hot code later.    │
  │ • Compiler memory lives in-process. │ • Lazy compilation: compile a function │
  │ • Compilation pauses (jitter) hurt  │   only when first called.              │
  │   tail latency.                     │ • Background compile threads.          │
  │ • Security: writable+executable     │ • W^X discipline: write code, flip to  │
  │   memory is an attack surface.      │   read+execute, never both at once.    │
  │ • Can't run where runtime codegen   │ • (none — use AOT there.)              │
  │   is forbidden (iOS, some consoles).│                                        │
  └─────────────────────────────────────┴────────────────────────────────────────┘
```

The central tension, which drives all JIT architecture:

```
   ┌────────────────────────────────────────────────────────────────────────┐
   │ Every millisecond spent COMPILING is a millisecond NOT spent RUNNING.  │
   │ But better-compiled code RUNS faster. The art of a JIT is spending     │
   │ compile time only where it pays off — i.e., on hot code.               │
   └────────────────────────────────────────────────────────────────────────┘

       compile cheaply      ◀────────────────────▶      compile thoroughly
       (fast start,                                       (slow start,
        slow steady state)                                 fast steady state)
                         tiered compilation walks this line
```

---

## 4. The in-process execution model

A JIT must do, *at runtime and in memory*, what an AOT toolchain does on disk. Recall the
last chapter's punchline: **a JIT is a linker that runs at runtime and targets memory.**

```
   ┌──────────────────────── the JIT, in-process ──────────────────────────┐
   │                                                                       │
   │  IR (Module) ─▶ optimize ─▶ backend codegen ─▶ machine code BYTES     │
   │                                                     │                 │
   │                                                     ▼                 │
   │  allocate memory pages (mmap), write the bytes there                  │
   │                                                     │                 │
   │                                                     ▼                 │
   │  RESOLVE symbols: this code calls printf, sin, other JIT'd fns —      │
   │  find their addresses (in libc, in the host process, in the JIT)      │
   │                                                     │                 │
   │                                                     ▼                 │
   │  apply RELOCATIONS: patch the call targets with real addresses        │
   │                                                     │                 │
   │                                                     ▼                 │
   │  mark pages EXECUTABLE (mprotect RX), flush instruction cache         │
   │                                                     │                 │
   │                                                     ▼                 │
   │  hand back the function's address as a pointer ──▶ CALL IT            │
   └───────────────────────────────────────────────────────────────────────┘
```

Every step has an AOT analog:

```
   AOT (on disk, offline)            JIT (in memory, at runtime)
   ─────────────────────             ───────────────────────────────
   emit .o                           emit bytes into mmap'd pages
   linker merges sections            JIT places sections in memory
   linker resolves symbols           JIT resolver finds addresses
   linker patches relocations        JIT patches relocations in place
   loader marks pages executable     JIT mprotects pages RX
   OS jumps to entry                 you call the function pointer
```

Same concepts, different *when* and *where*. This is why understanding AOT linking made you
ready for ORC.

---

## 5. Where the JIT reuses AOT machinery

Crucially, the JIT does **not** reimplement optimization or codegen. It calls the *same*
`PassManager` and the *same* `TargetMachine` backend you used for AOT — it just sends the
output bytes to memory instead of a file.

```
        frontend → Module
                     │
                     ▼
        optimize (SAME new-PM pipeline as AOT, ch.02.02)
                     │
                     ▼
        codegen via TargetMachine (SAME backend as AOT, ch.03.02)
                     │
          ┌──────────┴───────────┐
          ▼                      ▼
   AOT: write bytes        JIT: keep bytes in memory,
   to a .o file            link & relocate in-process,
                           return a function pointer
```

LLVM's JIT framework that orchestrates the in-memory linking is **ORC** (On-Request
Compilation). The next chapter dissects it. For now, the takeaway:

```
   ┌─────────────────────────────────────────────────────────────────────┐
   │ A JIT = (your frontend) + (LLVM's optimizer) + (LLVM's backend) +   │
   │         ORC's in-memory dynamic linker + a function-pointer call.   │
   │ Only the last two parts are "new" relative to AOT.                  │
   └─────────────────────────────────────────────────────────────────────┘
```

---

## 6. Real-world JITs and what they teach

```
  ┌───────────────┬────────────────────────────────────────────────────────────┐
  │ JVM HotSpot   │ interpret bytecode first; C1 (fast) then C2 (optimizing)   │
  │               │ compile hot methods. The canonical tiered JIT + deopt.     │
  │ V8 (JS)       │ Ignition interpreter → TurboFan optimizer; speculate on    │
  │               │ types, deopt on guard failure.                             │
  │ LuaJIT        │ trace-based: record hot loops as linear traces, compile.   │
  │ .NET CLR      │ JITs IL to native on first call (also has AOT: ReadyToRun).│
  │ PostgreSQL    │ uses LLVM ORC to JIT expression evaluation for queries.    │
  │ Julia         │ LLVM-based; JITs specialized native code per argument type.│
  │ lli / Cling   │ run/REPL LLVM IR / C++ directly via ORC.                   │
  └───────────────┴────────────────────────────────────────────────────────────┘
```

The recurring patterns across all of them — **tiering, lazy compilation, speculation +
deoptimization** — are exactly what LLVM ORC is designed to support, and what we cover in
[04-advanced-jit.md](04-advanced-jit.md).

---

## 7. A note on LLVM's JIT history (so old tutorials don't confuse you)

```
   timeline of LLVM JIT APIs:
   ───────────────────────────────────────────────────────────────────────
   (old)  ExecutionEngine / "the legacy JIT"   — removed
          MCJIT                                — works, but monolithic, no lazy
          ORCv1                                — layered, but clunky APIs, removed
   (now)  ORCv2  ── LLJIT (batteries-included) and the raw layer APIs
                    THIS is what you use today.
```

```
   ┌─────────────────────────────────────────────────────────────────────┐
   │ If a tutorial mentions `ExecutionEngine`, `MCJIT`, or ORCv1 layer   │
   │ classes with `addModule(...)` returning handles — it's outdated. Use│
   │ ORCv2: `LLJIT` for the easy path, or `ExecutionSession` + layers for│
   │ the custom path. We use ORCv2 throughout.                           │
   └─────────────────────────────────────────────────────────────────────┘
```

---

## Mental model checkpoint

1. State the defining property of JIT in terms of *when* and *where* compilation happens.
2. List three pieces of runtime information a JIT can exploit that AOT cannot.
3. What is the central time tension in a JIT, and what technique walks that line?
4. Recite the in-process execution steps from "machine code bytes" to "call the pointer."
5. Map each JIT step to its AOT linking analog.
6. Which parts of a JIT are *shared* with AOT, and which are genuinely new?
7. What is W^X and why does a JIT care?
8. Which LLVM JIT API is current, and which names signal an outdated tutorial?

Next → [02-orc-architecture.md](02-orc-architecture.md)

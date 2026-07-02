# 04.02 · ORC v2 Architecture

> ORC ("On-Request Compilation") is LLVM's modern JIT framework. It looks intimidating
> because it has many small classes, but the model is clean once you see it: a set of
> **layers** that transform code, **JITDylibs** that act like in-memory shared libraries, and
> a **symbol-resolution** mechanism that mirrors the dynamic linker. We diagram every piece.

---

## 1. The big picture

```
   ┌──────────────────────────── ExecutionSession ──────────────────────────────┐
   │  (the "JIT universe": owns everything, coordinates symbol lookup)          │
   │                                                                            │
   │   ┌─────────────┐   ┌─────────────┐        JITDylibs = in-memory .so's     │
   │   │ JITDylib    │   │ JITDylib    │        (namespaces of symbols)         │
   │   │  "main"     │   │  "runtime"  │                                        │
   │   │  fib, main  │   │  printf,sin │                                        │
   │   └─────────────┘   └─────────────┘                                        │
   │                                                                            │
   │   LAYER STACK (top = highest abstraction, code flows DOWN):                │
   │   ┌─────────────────────────────────────────────────────────────────────┐  │
   │   │ (your code adds IR Modules at the top)                              │  │
   │   │   ▼                                                                 │  │
   │   │ IRTransformLayer   — run optimization passes on each Module         │  │
   │   │   ▼                                                                 │  │
   │   │ IRCompileLayer     — compile IR → object (via TargetMachine)        │  │
   │   │   ▼                                                                 │  │
   │   │ ObjectLinkingLayer — link object into memory (JITLink), relocate    │  │
   │   │   ▼                                                                 │  │
   │   │ (machine code now live in executable memory; address resolvable)    │  │
   │   └─────────────────────────────────────────────────────────────────────┘  │
   │                                                                            │
   │   MangleAndInterner, SymbolStringPool, ExecutorProcessControl ...          │
   └────────────────────────────────────────────────────────────────────────────┘
```

Read it top-down: you hand IR in at the top; it flows down through layers (optimize →
compile → link-into-memory); at the bottom it's runnable machine code whose symbols live in a
JITDylib, ready to be looked up and called.

---

## 2. The core objects, one at a time

### ExecutionSession — the universe

```
   ExecutionSession (ES)
     • owns all JITDylibs, the symbol string pool, the error-reporting context
     • the central authority for symbol LOOKUP across the whole JIT
     • essentially "the JIT process model"
```

### JITDylib — an in-memory dynamic library

```
   A JITDylib is a NAMESPACE of symbols, exactly like a shared library (.so):
     • holds definitions (symbols → addresses, once materialized)
     • has a LINK ORDER: an ordered list of other JITDylibs to search for
       symbols it doesn't define (like a library's dependency list)
     • you can have several: e.g. "main" for user code, plus one exposing
       the host process's symbols (printf, malloc) for resolution.
```

```
   JITDylib "main"  ── link order ──▶  JITDylib "process"  (host symbols)
     defines: fib, square              defines: printf, sin, malloc (from this process)
     uses:    printf  ───────────────────────▶ found via link order
```

### Layers — composable code transformers

Each layer takes a unit of code, does one job, and passes it to the layer below.

```
  ┌────────────────────┬────────────────────────────────────────────────────┐
  │ IRTransformLayer   │ apply an IR transform (your optimization pipeline) │
  │                    │ to each Module as it descends. Optional but common.│
  ├────────────────────┼────────────────────────────────────────────────────┤
  │ IRCompileLayer     │ compile a Module to an object buffer using a       │
  │                    │ TargetMachine (the SAME backend as AOT).           │
  ├────────────────────┼────────────────────────────────────────────────────┤
  │ ObjectLinkingLayer │ link the object into the executor's memory:        │
  │ (uses JITLink)     │ allocate pages, apply relocations, register        │
  │                    │ exception/TLS info, mark executable.               │
  │                    │ (Older RTDyldObjectLinkingLayer used RuntimeDyld.) │
  └────────────────────┴────────────────────────────────────────────────────┘
```

### MaterializationUnit & the lazy core

This is ORC's cleverest idea and the source of "On-Request":

```
   When you "add" a Module, ORC does NOT compile it immediately. Instead it
   records a PROMISE: "JITDylib `main` can provide symbols {fib, square};
   when someone actually NEEDS one, run this MaterializationUnit to produce it."

   add Module ──▶ register symbol NAMES as "available"  (not yet compiled)
   lookup("fib") ──▶ triggers MATERIALIZATION ──▶ NOW the layers run:
                     transform → compile → link → address ready ──▶ returned
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ "On-Request Compilation": symbols are compiled lazily, the first time   │
   │ they are looked up (or called). This is how ORC supports compiling      │
   │ only what's actually used — the foundation for lazy & tiered JITs.      │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 3. Symbol resolution: how a JIT'd call finds its target

When code calls `printf`, the JIT must supply `printf`'s address. ORC resolves symbols by
searching JITDylibs in link order, and a special **generator** can expose the host process's
own symbols.

```
   lookup("printf") in JITDylib "main"
       │  not defined here → follow link order
       ▼
   JITDylib "process"
       │  has a DynamicLibrarySearchGenerator that exposes THIS process's
       │  exported symbols (it dlsym's the running executable + loaded libs)
       ▼
   finds &printf in the linked-in libc ──▶ returns its address
       │
       ▼
   relocation in the JIT'd code patched with printf's address ──▶ call works
```

The generator that makes host symbols visible (you'll add this in every real JIT):

```cpp
// Expose the host process's symbols (printf, sin, malloc, ...) to the JIT.
auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
    DataLayout.getGlobalPrefix());
MainJITDylib.addGenerator(std::move(gen.get()));
```

```
   ┌────────────────────────────────────────────────────────────────────────┐
   │ A GENERATOR is "what to do when a symbol is requested but not yet      │
   │ defined." GetForCurrentProcess says: try to find it among the host     │
   │ process's symbols. This is how JIT'd code calls into libc / your app.  │
   └────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Mangling and the SymbolStringPool

Symbol names must match the platform's name mangling (e.g. macOS prefixes `_`). ORC interns
names for fast comparison.

```
   "printf"  ──[Mangler, per DataLayout]──▶  "_printf" (macOS) or "printf" (linux)
            ──[SymbolStringPool interns]──▶  a unique pointer used as the key
```

```cpp
llvm::orc::MangleAndInterner Mangle(ES, DataLayout);
auto sym = Mangle("printf");   // returns an interned, platform-mangled name
```

You rarely touch this directly with `LLJIT` (it does it for you), but it explains the
occasional "_" prefix in lookups on macOS.

---

## 5. ExecutorProcessControl & out-of-process JITs

ORC abstracts *where the code runs* behind **ExecutorProcessControl (EPC)**:

```
   IN-PROCESS (default):                 OUT-OF-PROCESS (remote):
   ──────────────────────                ──────────────────────────────
   JIT compiles AND runs code in         JIT compiles in process A; the code
   the same process. Simple, fast.       is shipped to and run in process B
                                          (another process, sandbox, or even
                                          a different MACHINE/architecture).

        ┌────────────────┐                    ┌────────────────┐  ┌────────────────┐
        │ JIT + code     │                    │ JIT (compiler) │  │ executor       │
        │ same process   │                    │  process A     │──│ process B      │
        └────────────────┘                    └────────────────┘  │  runs the code │
                                                                  └────────────────┘
```

```
   Why remote? security isolation (a crash/exploit in JIT'd code doesn't
   take down the compiler/host), cross-target JIT (compile on x86, run on an
   embedded ARM board), and resource separation. EPC + ORC-RT make the memory
   management & symbol lookup work across the process boundary transparently.
```

For most uses you'll use the in-process default (`SelfExecutorProcessControl`), but knowing
EPC exists explains ORC's otherwise-surprising indirection.

---

## 6. How the pieces collaborate (a full lookup trace)

Putting it together — what happens on `jit.lookup("fib")` then calling it:

```
   (1) ES.lookup({MainJD}, "fib")
         │
   (2) MainJD: "fib" is a not-yet-materialized symbol I promised."
         │  trigger its MaterializationUnit
         ▼
   (3) IRTransformLayer: run -O2 passes on fib's Module
         ▼
   (4) IRCompileLayer: TargetMachine compiles IR → object buffer
         ▼
   (5) ObjectLinkingLayer (JITLink): allocate exec memory, write bytes,
         resolve fib's external refs (e.g. recursive fib, printf) via
         link order + generators, patch relocations, mark RX
         ▼
   (6) "fib" now has a concrete address in executable memory
         │
   (7) lookup returns that address as an ExecutorAddr
         ▼
   (8) cast to double(*)(double), call it — runs native code
```

```
   You see all of ORC's concepts in this one trace:
   ExecutionSession (drives lookup) · JITDylib (namespace + link order) ·
   MaterializationUnit (lazy compile) · the layer stack (transform→compile→
   link) · generators/link-order (symbol resolution) · ExecutorAddr (result).
```

---

## 7. LLJIT — the assembled, batteries-included JIT

You usually don't wire layers by hand. **LLJIT** is ORC's pre-assembled JIT with sensible
defaults (the layer stack above, a main JITDylib, a `TargetMachine` for the host). The next
chapter uses it:

```
   LLJIT  =  ExecutionSession
           + a main JITDylib (with a process-symbols generator you add)
           + IRCompileLayer (host TargetMachine)
           + ObjectLinkingLayer (JITLink/RuntimeDyld)
           + the DataLayout & Mangler wired up
   You: addIRModule(...) then lookup(...). Done.
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ Use LLJIT for 95% of cases. Drop to raw ExecutionSession + custom       │
   │ layers only when you need lazy/tiered/remote behavior LLJIT's defaults  │
   │ don't give you (and even then, LLLazyJIT and customization hooks exist).│
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## Mental model checkpoint

1. What does the ExecutionSession own and coordinate?
2. In what sense is a JITDylib "an in-memory shared library," and what is its link order for?
3. Name the three core layers in order and what each does.
4. What does "On-Request Compilation" mean, and which object implements the laziness?
5. How does JIT'd code calling `printf` actually find `printf`? What's a generator?
6. Why does symbol mangling/interning exist in ORC?
7. What is ExecutorProcessControl, and give two reasons to run code out-of-process.
8. What is LLJIT, and when would you bypass it for raw layers?

Next → [03-building-a-jit.md](03-building-a-jit.md)

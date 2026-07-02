# 03.01 · AOT Compilation — Theory

> Ahead-Of-Time compilation is the classic model: translate the *entire* program to native
> machine code *now*, store it on disk, run it *later* — possibly on another machine. This
> is what `clang`, `gcc`, `rustc`, and `go build` do. We establish the model, its trade-offs,
> and exactly where LLVM fits.

---

## 1. The defining characteristic

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │  AOT:  compilation happens BEFORE execution, as a SEPARATE step,        │
   │        producing a persistent artifact (object file / executable)       │
   │        that is run later by a different process (maybe on a different   │
   │        machine, definitely at a different time).                        │
   └─────────────────────────────────────────────────────────────────────────┘
```

The timeline makes it concrete:

```
   COMPILE TIME (developer's machine, once)        RUN TIME (user's machine, many times)
   ─────────────────────────────────────────       ─────────────────────────────────────
   source ─▶ IR ─▶ opt ─▶ machine code ─▶ .o        load executable ─▶ OS maps it ─▶ CPU runs
                                  │                   (NO compiler present here!)
                                  ▼
                          link ─▶ executable ────────────────────────────────▶ ./program
                                  (stored on disk)
```

The compiler is *gone* by the time the program runs. All the work — parsing, optimization,
codegen — was front-loaded. The shipped artifact is pure machine code plus metadata.

---

## 2. AOT vs interpretation vs JIT (the spectrum)

```
   PURE INTERPRETER          JIT                          AOT
   ─────────────────         ───────────────────          ────────────────────
   read source/bytecode      compile to native            compile to native
   & execute on the fly,     code at RUNTIME, in          code BEFORE runtime,
   no native code            this process, then run        ship the native code

   startup: instant          startup: warmup cost         startup: instant
   peak speed: slow          peak speed: high (can use     peak speed: high
                             runtime info!)                (only static info)
   ships: source/bytecode    ships: source/bytecode +     ships: native binary
                             a compiler
   examples: CPython,        examples: JVM, V8,            examples: clang, gcc,
   Ruby MRI                  LuaJIT, .NET                  rustc, go, swiftc
```

The fundamental trade in one line:

```
   AOT pays the compile cost ONCE, OFFLINE, but can only use information
   known statically. JIT pays it AT RUNTIME, REPEATEDLY, but can exploit
   actual runtime behavior (hot paths, observed types, real values).
```

---

## 3. Strengths and weaknesses of AOT

```
  ┌────────────────────────────────────┬────────────────────────────────────────┐
  │ STRENGTHS                          │ WEAKNESSES                             │
  ├────────────────────────────────────┼────────────────────────────────────────┤
  │ • Zero startup/warmup cost.        │ • No runtime profile: must guess hot   │
  │   The binary runs immediately.     │   paths, branch probabilities.         │
  │ • No compiler shipped → smaller    │ • Can't specialize to actual inputs    │
  │   runtime footprint, fewer deps.   │   or observed dynamic types.           │
  │ • Predictable performance (no JIT  │ • Must target a fixed ISA baseline     │
  │   pauses, no compilation jitter).  │   (or ship multiple variants / use     │
  │ • Whole-program opts via LTO.      │   runtime CPU dispatch).               │
  │ • Easy to inspect/secure/sign the  │ • Long full rebuilds for big programs. │
  │   final artifact.                  │ • Cross-compilation needs care (right  │
  │ • Works in restricted environments │   triple, sysroot, libs).              │
  │   that forbid runtime codegen      │                                        │
  │   (W^X, iOS App Store, etc.).      │                                        │
  └────────────────────────────────────┴────────────────────────────────────────┘
```

> **Profile-Guided Optimization (PGO)** is AOT's answer to "no runtime profile": do an
> instrumented run first to *collect* a profile, then recompile using it. It recovers much of
> JIT's runtime-awareness while staying AOT. (`clang -fprofile-generate` then
> `-fprofile-use`.)

---

## 4. Where LLVM fits in an AOT compiler

You build the front; LLVM builds the rest. The AOT-specific piece is the **last step**:
turning an optimized `Module` into a native **object file** on disk via a `TargetMachine`.

```
   YOUR CODE                         LLVM                               OS/toolchain
   ───────────                       ──────────────────────             ──────────────
   frontend → Module                 optimize Module (PassMgr)
                                     TargetMachine::addPassesToEmitFile
                                       → SelectionDAG/GlobalISel
                                       → register allocation
                                       → MC layer encodes bytes
                                       → write .o to disk          ───▶  linker (ld/lld)
                                                                          combines .o +
                                                                          libs → executable
```

The two AOT-defining LLVM concepts, covered next chapter:

```
   • TargetMachine  — embodies "the machine we compile for": its ISA, ABI,
                      register set, scheduling model. Created from a target triple.
   • Object emission — TargetMachine drives the backend to write a .o file
                      (ELF on Linux, Mach-O on macOS, COFF on Windows).
```

After that, **linking** (final chapter of this section) combines object files and libraries
into a runnable executable. LLVM provides `lld`, or you can shell out to the system linker.

---

## 5. The artifact chain: from IR to a runnable program

```
   square.ll  (LLVM IR, text)
      │  opt -O2            (optimize)
      ▼
   square.opt.ll
      │  llc -filetype=obj  (TargetMachine: codegen + emit)
      ▼
   square.o   (relocatable object: machine code + symbols + relocations)
      │  link with crt0, libc, other .o's       (resolve symbols, lay out memory)
      ▼
   square     (executable: ELF/Mach-O with entry point, segments)
      │  exec()             (OS loads segments, jumps to _start → main)
      ▼
   running process
```

Each artifact is *less abstract and more committed* than the last:

```
   .ll   target-ish but portable-ish IR
   .o    committed to an ISA & object format, but NOT to addresses (relocatable)
   exe   committed to a layout, entry point, and (for static) all code present
```

We dig into objects, relocations, and linking in
[04-linking-and-runtime.md](04-linking-and-runtime.md) — they explain *why* the `.o` → `exe`
step exists at all.

---

## 6. Static vs dynamic, and the runtime

Even a "simple" AOT binary depends on a **runtime**:

```
   ┌──────────────────────────────────────────────────────────────────────────┐
   │ Your main() does NOT run first. The C runtime startup (crt0/_start)      │
   │ runs first: it sets up the stack, argc/argv, calls global constructors,  │
   │ THEN calls main(), and on return calls exit().                           │
   └──────────────────────────────────────────────────────────────────────────┘

   _start (from crt0)
      │ set up stack, argc/argv/envp
      │ run .init / global ctors
      ▼
   main(argc, argv)        ◀── your code
      │ return
      ▼
   exit()  → run dtors, flush stdio, _exit syscall
```

And libraries link two ways:

```
   STATIC linking:  copy library code INTO the executable.
     + self-contained, no runtime deps   − bigger, no shared updates
   DYNAMIC linking: leave references; the loader resolves them at startup.
     + small, shared libc in memory      − needs the .so/.dylib present at run
```

These choices are made at the **link** step, not by LLVM's codegen. Your AOT driver decides
which by how it invokes the linker.

---

## Mental model checkpoint

1. State the defining property of AOT in terms of *when* compilation happens.
2. Place interpreter / JIT / AOT on the startup-cost vs peak-speed spectrum.
3. Give two things AOT can't know that a JIT can, and name AOT's partial remedy.
4. Which single LLVM concept is "the machine we compile for," and what creates it?
5. List the artifact chain `.ll → .o → exe` and what each commits to.
6. Why doesn't your `main()` run first?
7. Static vs dynamic linking: one advantage each.

Next → [02-target-machine-codegen.md](02-target-machine-codegen.md)

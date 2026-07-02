# 07.01 · Capstone — One Language, Three Backends

> Time to tie everything together. We take the **Toy** language from section 01 and ship it
> three ways from the *same frontend*: as an **AOT** compiler, as a **JIT** REPL, and via an
> **MLIR** pipeline. Seeing the shared spine and the three divergent tails cements the entire
> guide.

---

## 1. The shared spine

All three share the frontend (lexer → parser → AST) from section 01. They diverge only at
"what do we do with the produced IR?"

```
   prog.toy
      │  Lexer (01.01)
      ▼
   tokens
      │  Parser (01.02)
      ▼
   AST
      │
      ├──────────────── codegen to LLVM IR (01.03) ────────────────┐
      │                                                            │
      │                                          codegen to MLIR (05.02/04)
      ▼                                                            ▼
   llvm::Module  ◀─────────── (MLIR path lowers + translates to here, 05.05)
      │
      │  optimize (02.02) — SHARED
      ▼
   optimized llvm::Module
      │
      ├──────────────┬──────────────────────┐
      ▼              ▼                       ▼
   AOT (§2)        JIT (§3)              (MLIR fed in above) (§4)
   .o → link       ORC → call
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ ~80% of the code (frontend + optimization) is shared verbatim. The      │
   │ three "backends" are thin: AOT ≈ 40 lines (object emit + link), JIT ≈   │
   │ 30 lines (LLJIT add+lookup+call), MLIR ≈ a dialect + lowering passes.   │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Backend A — AOT (compile now, run later)

Reusing chapter 03.03's driver. The shape:

```cpp
// toyc-aot.cpp
int main(int argc, char **argv) {
  InitializeModule();
  runFrontend(openFile(argv[1]));        // section 01 frontend → TheModule
  verifyModule(*TheModule, &errs());

  optimizeModule(*TheModule, /*O=*/2);   // section 02.02 — SHARED
  synthesizeMain(*TheModule, ...);       // entry point (03.03)
  moduleToObjectFile(*TheModule, "out.o");// section 03.02 → object on disk
  linkExecutable("out.o", "prog");       // section 03.04 → executable
  // compiler exits; nothing of ours runs at program runtime.
}
```

```
   $ ./toyc-aot prog.toy
   wrote out.o ; linking → prog
   $ ./prog                 # AOT: native binary, no compiler present
   50.000000
```

```
   Characteristics realized:
     • instant startup, no compiler shipped (ch.03.01)
     • could cross-compile by swapping the triple (ch.03.02 §6)
     • could BOLT the binary with a profile for more speed (ch.03.05)
```

---

## 3. Backend B — JIT REPL (compile now, run now)

Reusing chapter 04.03's REPL. The shape:

```cpp
// toyc-jit.cpp
int main() {
  InitializeNativeTarget(); InitializeNativeTargetAsmPrinter();
  TheJIT = cantFail(LLJITBuilder().create());
  addProcessSymbolsGenerator(TheJIT);    // so toy code can call printf/sin (04.03)
  InitializeModule(); getNextToken();
  MainLoop();   // each top-level expr → codegen → addIRModule → lookup → CALL
}
```

```
   $ ./toyc-jit
   ready> def fib(n) if n<2 then n else fib(n-1)+fib(n-2);
   ready> fib(10);
   Evaluated to 55.000000          ◀ compiled to native & executed IMMEDIATELY
   ready> fib(20);
   Evaluated to 6765.000000        ◀ reuses the already-JIT'd fib
```

```
   Characteristics realized:
     • interactive: define & call in the same process (ch.04.01)
     • cross-module symbol resolution via the JITDylib (ch.04.02)
     • could add an IRTransformLayer for a cheap pipeline (ch.04.03 §5)
     • could go tiered: baseline fib, reoptimize when hot (ch.04.04)
```

The *only* real difference from AOT: instead of writing `out.o`, we `addIRModule` to ORC,
`lookup` the symbol, and call the returned pointer. Same frontend, same optimizer.

---

## 4. Backend C — via MLIR (model high, lower down)

For a language as simple as Toy, MLIR is overkill — but the *exercise* shows the on-ramp. We
codegen to a small `toy` dialect (or directly to `arith`/`func`/`scf`), then lower to LLVM IR
and reuse A or B.

```
   AST
     │ codegen to MLIR (OpBuilder, ch.05.02): func.func + arith + scf ops
     ▼
   toy.mlir  (arith/func/scf dialects)
     │ mlir-opt: -convert-scf-to-cf -convert-arith-to-llvm
     │           -convert-func-to-llvm -reconcile-unrealized-casts  (ch.05.03)
     ▼
   llvm-dialect.mlir
     │ mlir-translate --mlir-to-llvmir  (ch.05.05)
     ▼
   out.ll  (LLVM IR)  ──▶ now identical to backend A or B!
```

```cpp
// toyc-mlir.cpp (sketch)
int main(int argc, char **argv) {
  MLIRContext ctx;
  ctx.loadDialect<func::FuncDialect, arith::ArithDialect, scf::SCFDialect>();
  ModuleOp module = codegenToMLIR(parseFile(argv[1]), ctx);   // ch.05.02 style

  PassManager pm(&ctx);                       // ch.05.03
  pm.addPass(createConvertSCFToCFPass());
  pm.addPass(createArithToLLVMConversionPass());
  pm.addPass(createConvertFuncToLLVMPass());
  pm.addPass(createReconcileUnrealizedCastsPass());
  if (failed(pm.run(module))) return 1;

  llvm::LLVMContext llvmCtx;
  auto llvmModule = toLLVMIR(module, llvmCtx);  // ch.05.05 → llvm::Module
  // From here: optimizeModule + (moduleToObjectFile | LLJIT) — backends A or B!
}
```

```
   Characteristics realized:
     • domain ops could be optimized at a high level before lowering (05.01)
     • multiple targets (CPU/GPU) reachable from one dialect (05.05 §6)
     • for Toy specifically: more machinery than payoff — which is the LESSON.
       Use MLIR when your domain has structure worth modeling (05.05 §6 table).
```

---

## 5. The three, side by side

```
  ┌────────────────┬───────────────────┬───────────────────┬───────────────────┐
  │                │ AOT (A)           │ JIT (B)           │ MLIR (C)          │
  ├────────────────┼───────────────────┼───────────────────┼───────────────────┤
  │ frontend       │ section 01        │ section 01        │ section 01        │
  │ produce IR     │ → llvm::Module    │ → llvm::Module    │ → MLIR → llvm::Mod│
  │ optimize       │ 02.02 (-O2)       │ 02.02 (cheap+tier)│ MLIR opts + 02.02 │
  │ deliver        │ object → link     │ ORC → fn ptr      │ lower → A or B    │
  │ runs           │ later, separate   │ now, in-process   │ either            │
  │ extra powers   │ BOLT/PGO/LTO      │ tiering/deopt/OSR │ multi-target,     │
  │                │                   │                   │ domain opts       │
  │ new code lines │ ~40               │ ~30               │ dialect+passes    │
  └────────────────┴───────────────────┴───────────────────┴───────────────────┘
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ THE LESSON OF THE WHOLE GUIDE, PROVEN: you wrote ONE compiler. The      │
   │ frontend and optimizer are shared. AOT, JIT, and MLIR are three         │
   │ endings to the same story, chosen by what you do with the Module —      │
   │ write it, call it, or model-and-lower into it.                          │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 6. Suggested exercises (to make it yours)

```
  1. Add a `for` loop to Toy: extend the lexer (01.01), parser (01.02),
     and codegen a loop with PHIs (ch.02.01 makeSumTo is your template).
  2. Add mutable variables with `var`/`=`: use the alloca + mem2reg trick
     (ch.00.04 §5, ch.01.03 §7). Confirm mem2reg builds PHIs for you.
  3. AOT: add a `--emit-llvm` and `--emit-asm` flag (call print / llc-equiv).
  4. JIT: add a tiered mode — compile at -O0 first, recompile hot functions
     at -O2 in a background thread, hot-swap the pointer (ch.04.04 §3).
  5. JIT: add ResourceTrackers so redefining a function works (ch.04.03 §4).
  6. MLIR: define a real `toy` dialect in ODS (ch.05.04) with a canonicalizer
     (e.g. constant folding) and watch -canonicalize simplify your IR.
  7. Backend: run `llc -stop-after=greedy` on your generated IR and read the
     MIR before/after register allocation (ch.06.03 §8).
  8. Measure: AOT-compile fib, then BOLT it with a perf profile (ch.03.05);
     compare to the JIT'd tiered version's steady-state speed.
```

---

## 7. Where to go next (beyond this guide)

```
  • Read LLVM's own Kaleidoscope tutorial chapters 1–9 (you now understand
    every line, and can spot where it's pre-opaque-pointer / pre-ORCv2).
  • Read MLIR's "Toy" tutorial chapters 1–7 (the dialect + lowering you sketched).
  • Add debug info (DWARF) so gdb/lldb can step your language (DIBuilder).
  • Implement a garbage collector / exception model if your language needs them
    (LLVM has statepoints, the Itanium EH ABI, etc.).
  • Study a real LLVM-based language: Rust (rustc_codegen_llvm), Julia, Swift,
    Clang itself — all use the exact pipeline you built.
  • Contribute a small pass or fix to LLVM/MLIR; you have the mental model now.
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ You set out to become an LLVM expert. You now have: a mental model of   │
   │ the three-phase design; fluency in IR and SSA; a working frontend;      │
   │ command of the pass infrastructure; the ability to build AOT, JIT, and  │
   │ MLIR compilers; knowledge of post-link tooling (BOLT et al.); and a     │
   │ tour of the backend's guts. The rest is practice and depth. Go build.   │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## Mental model checkpoint (the whole guide)

1. What fraction of the three backends is shared, and what specifically differs?
2. For Toy, why is the AOT/JIT difference only "write the Module vs call into it"?
3. Why is MLIR overkill for Toy, and what would justify it?
4. Recite the single unifying sentence: AOT, JIT, and MLIR are ____.
5. Pick one exercise and sketch which chapters you'd revisit to do it.

← Back to the [guide index](../README.md) · Buildable code in [../examples/README.md](../examples/README.md)

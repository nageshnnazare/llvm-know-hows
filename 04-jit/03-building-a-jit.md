# 04.03 · Building a JIT — From LLJIT to Custom Layers

> Now we write real JITs. First the easy, production path with **LLJIT** (a few lines), then
> a **REPL** that JIT-compiles Toy expressions interactively, then a **hand-built JIT** from
> raw ORC layers so you understand what LLJIT does for you. All code is current ORCv2.

---

## 1. The minimal LLJIT

The smallest useful JIT: build a Module, add it, look up a symbol, call it.

```cpp
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
using namespace llvm;
using namespace llvm::orc;

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  // 1. The JIT uses the backend → initialize native target codegen.
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();

  // 2. Build the JIT (LLJITBuilder picks the host TargetMachine, builds layers).
  auto jitOrErr = LLJITBuilder().create();
  if (!jitOrErr) { logAllUnhandledErrors(jitOrErr.takeError(), errs()); return 1; }
  std::unique_ptr<LLJIT> J = std::move(*jitOrErr);

  // 3. Create a Module with the JIT's DataLayout (MUST match the JIT's target).
  auto ctx = std::make_unique<LLVMContext>();
  auto M = std::make_unique<Module>("jit_demo", *ctx);
  M->setDataLayout(J->getDataLayout());

  IRBuilder<> b(*ctx);
  auto *i32 = b.getInt32Ty();
  auto *fn = Function::Create(FunctionType::get(i32, {i32}, false),
                              Function::ExternalLinkage, "times3", M.get());
  auto *bb = BasicBlock::Create(*ctx, "entry", fn);
  b.SetInsertPoint(bb);
  b.CreateRet(b.CreateMul(fn->getArg(0), b.getInt32(3)));

  // 4. Hand the Module to the JIT (wrapped as a ThreadSafeModule).
  if (auto err = J->addIRModule(ThreadSafeModule(std::move(M), std::move(ctx)))) {
    logAllUnhandledErrors(std::move(err), errs()); return 1;
  }

  // 5. Look up the symbol → triggers compilation → returns an address.
  auto sym = J->lookup("times3");
  if (!sym) { logAllUnhandledErrors(sym.takeError(), errs()); return 1; }

  // 6. Cast to a function pointer and CALL IT — native code, right now.
  auto *times3 = sym->toPtr<int(int)>();
  outs() << "times3(14) = " << times3(14) << "\n";   // 42
  return 0;
}
```

```
   The 6 steps of ANY LLJIT use:
   ┌───────────────────────────────────────────────────────────────────────┐
   │ init native target → create LLJIT → build Module (with JIT datalayout)│
   │ → addIRModule → lookup(symbol) → toPtr + call                         │
   └───────────────────────────────────────────────────────────────────────┘
```

Two non-negotiable details that bite beginners:

```
   (1) M->setDataLayout(J->getDataLayout())  — the Module's layout MUST equal
       the JIT's, or codegen/linking misbehaves.
   (2) ThreadSafeModule bundles the Module WITH its LLVMContext, so ORC can
       move it across threads safely. Don't keep using the context afterward.
```

---

## 2. Letting JIT'd code call libc (`printf`, `sin`)

If your IR calls `printf`, the JIT must resolve it. Add a generator that exposes the host
process's symbols to the main JITDylib:

```cpp
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
// after creating J:
auto &JD = J->getMainJITDylib();
JD.addGenerator(cantFail(
    DynamicLibrarySearchGenerator::GetForCurrentProcess(
        J->getDataLayout().getGlobalPrefix())));
```

```
   Now a JIT'd `call i32 @printf(...)` resolves like this:
   lookup printf in main JD → not defined → generator dlsym's the process →
   finds libc's printf (linked into THIS executable) → address patched in.
   (Tip: make sure printf isn't dead-stripped from your JIT host; reference
    it or link with -rdynamic / export-dynamic so it's in the symbol table.)
```

---

## 3. A Toy REPL that JIT-compiles each line

This is where JIT shines: an interactive loop that compiles and *runs* each entered
expression immediately. We reuse the section-01 frontend. Each top-level expression becomes
`double __anon_expr()` (recall chapter 01.02), which we JIT and call.

```cpp
// Toy JIT REPL (sketch; reuses Lexer/Parser/Codegen from section 01)
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
using namespace llvm; using namespace llvm::orc;

static std::unique_ptr<LLJIT> TheJIT;

// Per the chapter-01 codegen, these globals hold the current module/context:
extern std::unique_ptr<LLVMContext> TheContext;
extern std::unique_ptr<Module> TheModule;
void InitializeModule();   // (re)creates TheContext + TheModule + IRBuilder

static void HandleTopLevelExpr() {
  if (auto FnAST = ParseTopLevelExpr()) {
    if (FnAST->codegen()) {                         // emits @__anon_expr into TheModule
      // Move the current module into the JIT, then start a fresh one.
      auto TSM = ThreadSafeModule(std::move(TheModule), std::move(TheContext));
      cantFail(TheJIT->addIRModule(std::move(TSM)));
      InitializeModule();                            // fresh module for next line

      // Look up, call, print.
      auto sym = cantFail(TheJIT->lookup("__anon_expr"));
      double (*fp)() = sym.toPtr<double()>();
      fprintf(stderr, "Evaluated to %f\n", fp());

      // Remove the anonymous expr so the name is reusable next time.
      // (Use a ResourceTracker — see §4 — to remove just this module.)
    }
  } else getNextToken();
}

int main() {
  InitializeNativeTarget(); InitializeNativeTargetAsmPrinter();
  TheJIT = cantFail(LLJITBuilder().create());
  TheJIT->getMainJITDylib().addGenerator(cantFail(
      DynamicLibrarySearchGenerator::GetForCurrentProcess(
          TheJIT->getDataLayout().getGlobalPrefix())));
  InitializeModule();
  getNextToken();
  MainLoop();   // dispatches def/extern/top-level; top-level → HandleTopLevelExpr
}
```

```
   REPL flow per line:
   ───────────────────────────────────────────────────────────────────────
   "fib(10);"  ─lex/parse─▶ AST ─codegen─▶ @__anon_expr in a Module
        │
        ▼
   addIRModule(Module)  ── JIT records the promise; fresh Module started
        │
        ▼
   lookup("__anon_expr") ── triggers compile of this module (and any defs it
        │                    uses, like fib, resolved across prior modules)
        ▼
   call the function pointer ── prints "Evaluated to 55.0"
```

The subtlety: function *definitions* (`def fib...`) entered earlier must remain resolvable
when a later expression calls them. ORC handles this because each added module's symbols live
in the JITDylib; cross-module calls resolve through it. (Real Kaleidoscope keeps a map of
function prototypes so codegen can re-declare them in each new module.)

---

## 4. Managing JIT'd code lifetime: ResourceTrackers

In a REPL you add modules forever — you need to *remove* them (e.g., redefining a function).
ORC uses **ResourceTracker** to scope a set of symbols you can later free.

```cpp
auto RT = TheJIT->getMainJITDylib().createResourceTracker();
cantFail(TheJIT->addIRModule(RT, std::move(TSM)));   // associate module with RT
// ... use it ...
cantFail(RT->remove());   // free that module's code & symbols
```

```
   ResourceTracker = a handle to "everything added under it."
   remove() reclaims the executable memory and removes the symbols, so you
   can redefine `fib` or drop a one-shot __anon_expr. Without it, JIT memory
   grows unbounded and symbol redefinition errors out.
```

---

## 5. Optimizing in the JIT: adding an IRTransformLayer

LLJIT lets you install a transform run on every module before compilation — your optimization
pipeline (chapter 02.02), tuned for fast compiles:

```cpp
#include "llvm/Passes/PassBuilder.h"

TheJIT->getIRTransformLayer().setTransform(
  [](ThreadSafeModule TSM, const MaterializationResponsibility &)
      -> Expected<ThreadSafeModule> {
    TSM.withModuleDo([](Module &M) {
      // Build a quick pipeline (mem2reg + a few cleanups) — cheap for JIT latency.
      LoopAnalysisManager LAM; FunctionAnalysisManager FAM;
      CGSCCAnalysisManager CGAM; ModuleAnalysisManager MAM;
      PassBuilder PB;
      PB.registerModuleAnalyses(MAM); PB.registerCGSCCAnalyses(CGAM);
      PB.registerFunctionAnalyses(FAM); PB.registerLoopAnalyses(LAM);
      PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
      ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
      MPM.run(M, MAM);
    });
    return std::move(TSM);
  });
```

```
   Where it sits (from chapter 04.02's layer stack):
     addIRModule ─▶ [IRTransformLayer: YOUR passes here] ─▶ IRCompileLayer ─▶ link
   This is the JIT equivalent of optimizeModule() in the AOT driver — but you
   choose a CHEAPER pipeline to keep latency low (see ch.02.03 §6, ch.04.04).
```

---

## 6. The hand-built JIT (what LLJIT assembles for you)

To demystify LLJIT, here is the classic "KaleidoscopeJIT" structure: raw ORC layers wired by
hand. Study it once; then appreciate why you use LLJIT.

```cpp
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/Orc/ExecutorProcessControl.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
using namespace llvm; using namespace llvm::orc;

class MyJIT {
  std::unique_ptr<ExecutionSession> ES;
  DataLayout DL;
  MangleAndInterner Mangle;
  RTDyldObjectLinkingLayer ObjectLayer;   // link objects into memory
  IRCompileLayer CompileLayer;             // compile IR → objects
  JITDylib &MainJD;

public:
  MyJIT(std::unique_ptr<ExecutionSession> ES, JITTargetMachineBuilder JTMB, DataLayout DL)
    : ES(std::move(ES)), DL(std::move(DL)), Mangle(*this->ES, this->DL),
      ObjectLayer(*this->ES, []{ return std::make_unique<SectionMemoryManager>(); }),
      CompileLayer(*this->ES, ObjectLayer,
                   std::make_unique<ConcurrentIRCompiler>(std::move(JTMB))),
      MainJD(this->ES->createBareJITDylib("<main>")) {
    // Resolve host process symbols (printf, etc.).
    MainJD.addGenerator(cantFail(
        DynamicLibrarySearchGenerator::GetForCurrentProcess(DL.getGlobalPrefix())));
  }

  static Expected<std::unique_ptr<MyJIT>> Create() {
    auto EPC = SelfExecutorProcessControl::Create();      // in-process
    if (!EPC) return EPC.takeError();
    auto ES = std::make_unique<ExecutionSession>(std::move(*EPC));
    JITTargetMachineBuilder JTMB(ES->getExecutorProcessControl().getTargetTriple());
    auto DL = JTMB.getDefaultDataLayoutForTarget();
    if (!DL) return DL.takeError();
    return std::make_unique<MyJIT>(std::move(ES), std::move(JTMB), std::move(*DL));
  }

  const DataLayout &getDataLayout() const { return DL; }

  Error addModule(ThreadSafeModule TSM) {
    return CompileLayer.add(MainJD.getDefaultResourceTracker(), std::move(TSM));
  }
  Expected<ExecutorSymbolDef> lookup(StringRef Name) {
    return ES->lookup({&MainJD}, Mangle(Name.str()));
  }
};
```

Map it to chapter 04.02's diagram:

```
   ExecutionSession ES          ── the universe
   RTDyldObjectLinkingLayer     ── ObjectLinkingLayer (link into memory)
   IRCompileLayer + Concurrent  ── compile IR → object via TargetMachine
     IRCompiler(JTMB)
   MainJD + process generator   ── the in-memory .so + host-symbol resolution
   Mangle                        ── platform name mangling
   addModule / lookup            ── the public API LLJIT also exposes
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ That ~40 lines IS (a simplified) LLJIT. LLJIT just adds an              │
   │ IRTransformLayer slot, lazy-compilation options, better defaults, and   │
   │ JITLink instead of RuntimeDyld. Now you know what create() builds.      │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 7. Common pitfalls checklist

```
  ┌─────────────────────────────────────────────────────────────────────────┐
  │ ☐ Forgot InitializeNativeTarget*/AsmPrinter → "no compiler for triple". │
  │ ☐ Module DataLayout ≠ JIT DataLayout → bad codegen / link failures.     │
  │ ☐ Used the context after moving it into ThreadSafeModule → UB.          │
  │ ☐ JIT'd code calls printf but no process-symbol generator → unresolved. │
  │ ☐ printf got dead-stripped from the host → link with -rdynamic.         │
  │ ☐ Re-adding a Module with the same symbol w/o a ResourceTracker remove  │
  │   → "duplicate definition" error.                                       │
  │ ☐ Following an MCJIT/ExecutionEngine tutorial → wrong API era.          │
  └─────────────────────────────────────────────────────────────────────────┘
```

---

## Mental model checkpoint

1. List the six steps of using LLJIT.
2. Why must the Module's DataLayout equal the JIT's, and what is a ThreadSafeModule?
3. How do you let JIT'd code call `printf`? Why might `printf` still be unresolved?
4. In the REPL, what triggers compilation of a top-level expression?
5. What problem does a ResourceTracker solve in a long-running REPL?
6. Where in the layer stack does your optimization pipeline run, and why pick a cheap one?
7. Match the hand-built JIT's members to the ORC architecture diagram.

Next → [04-advanced-jit.md](04-advanced-jit.md)

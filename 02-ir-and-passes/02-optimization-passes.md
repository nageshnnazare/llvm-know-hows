# 02.02 · Optimization Passes — What They Do and How to Write One

> The "middle end" is a sequence of **passes** that transform IR into better IR. This chapter
> explains the pass model, the difference between analysis and transformation, the major
> passes you should recognize, and how to write your own pass in the **new PassManager**.

---

## 1. What a pass is

```
   A PASS is a unit of work over some IR scope. It either:
     • ANALYZES   — computes information, mutates nothing (e.g. dominator tree)
     • TRANSFORMS — rewrites the IR to be faster/smaller (e.g. dead-code elim)

           ┌──────────┐   pass    ┌──────────┐   pass    ┌──────────┐
   IR ────▶│ Module   │─────────▶ │ Module'  │─────────▶ │ Module'' │────▶ ...
           └──────────┘           └──────────┘           └──────────┘
            each pass sees the whole (or a slice) and improves it
```

Passes are scoped by what IR unit they operate on:

```
  ┌────────────────────┬───────────────────────────────────────────────────────┐
  │ Module pass        │ sees the entire module (all functions + globals)      │
  │ Function pass      │ runs once per function (most common)                  │
  │ Loop pass          │ runs per loop (loop-invariant code motion, unrolling) │
  │ CGSCC pass         │ per strongly-connected component of the call graph    │
  │                    │ (enables interprocedural work like inlining order)    │
  └────────────────────┴───────────────────────────────────────────────────────┘
```

---

## 2. The passes you must recognize

You don't need to memorize all ~200, but these are the workhorses. Knowing what they do lets
you read `-O2` output and build sensible custom pipelines.

```
  ┌───────────────┬────────────────────────────────────────────────────────────┐
  │ mem2reg       │ promote alloca/load/store to SSA registers + PHIs.         │
  │ (SROA)        │ THE pass that makes frontend alloca-style codegen clean.   │
  ├───────────────┼────────────────────────────────────────────────────────────┤
  │ instcombine   │ peephole: algebraic simplifications (x*2 → x<<1, x+0 → x). │
  │ reassociate   │ reorder associative ops to expose constants/CSE.           │
  │ gvn / earlycse│ global value numbering / common-subexpression elimination. │
  │ sccp          │ sparse conditional constant propagation (folds constants,  │
  │               │ removes provably-dead branches simultaneously).            │
  │ simplifycfg   │ clean up the CFG: merge blocks, remove dead/empty blocks,  │
  │               │ turn branches into selects, fold trivial conditions.       │
  │ dce / adce    │ dead-code elimination (aggressive variant uses liveness).  │
  ├───────────────┼────────────────────────────────────────────────────────────┤
  │ inline        │ replace a call with the callee's body (huge enabler).      │
  │ licm          │ loop-invariant code motion: hoist invariants out of loops. │
  │ loop-unroll   │ replicate loop bodies to cut branch overhead.              │
  │ loop-vectorize│ turn scalar loops into SIMD (<N x T>) operations.          │
  │ slp-vectorize │ combine independent scalar ops into vectors.               │
  │ tailcallelim  │ turn tail-recursion into loops.                            │
  └───────────────┴────────────────────────────────────────────────────────────┘
```

### Watch a few in action

`instcombine` peephole:

```
   before                        after
   ─────────────                 ─────────────
   %t = mul i32 %x, 2            %t = shl i32 %x, 1      ; cheaper
   %a = add i32 %y, 0            (replaced by %y)         ; identity removed
   %b = sub i32 %z, %z          (replaced by 0)
```

`sccp` (constant propagation + dead branch removal together):

```
   before                              after
   ─────────────────────────           ─────────────────
   %c = icmp eq i32 5, 5               ; %c is always true →
   br i1 %c, label %a, label %b        br label %a        ; %b becomes dead
```

`licm` hoisting an invariant out of a loop:

```
   before (recompute n*4 every iter)     after (hoisted once)
   ────────────────────────────────      ────────────────────────
   loop:                                  %inv = mul i32 %n, 4   ; before loop
     %t = mul i32 %n, 4   ; invariant!   loop:
     ...use %t...                          ...use %inv...
```

---

## 3. The two pass managers (and why the new one exists)

```
   LEGACY PassManager (deprecated)        NEW PassManager (use this)
   ─────────────────────────────          ──────────────────────────
   passes register dependencies           passes declare results;
   on each other dynamically;             a PassManager<IRUnit> is
   analyses recomputed implicitly;        templated by IR unit;
   harder to compose, slower.             analyses cached in an
                                          AnalysisManager, results
                                          shared & invalidated explicitly.
```

The new PM (NPM) is faster, more predictable, and the default since LLVM 13ish. Its core
objects:

```
   PassBuilder        builds standard pipelines (-O2 etc.) and parses pass strings
   ModuleAnalysisManager  (MAM)  caches module-level analyses
   FunctionAnalysisManager(FAM)  caches function-level analyses
   ModulePassManager  (MPM)  a sequence of module passes
   FunctionPassManager(FPM)  a sequence of function passes
```

```
        PassBuilder
            │ registers analyses into the managers, builds pipelines
            ▼
   ┌───────────────────────────────────────────────────┐
   │ ModulePassManager                                 │
   │   ├── (module passes...)                          │
   │   └── ModuleToFunctionPassAdaptor                 │
   │          └── FunctionPassManager                  │
   │                 ├── PromotePass (mem2reg)         │
   │                 ├── InstCombinePass               │
   │                 └── ...                           │
   └───────────────────────────────────────────────────┘
            │ run(Module, MAM)
            ▼
        transformed Module
```

---

## 4. Running standard pipelines from your code

The most common need: "optimize my Module like `-O2`." Here it is in the new PM:

```cpp
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/OptimizationLevel.h"
using namespace llvm;

void optimizeModule(Module &M, OptimizationLevel level = OptimizationLevel::O2) {
  // 1. Create the four analysis managers.
  LoopAnalysisManager     LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager    CGAM;
  ModuleAnalysisManager   MAM;

  // 2. The PassBuilder wires them up.
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);   // let managers see each other

  // 3. Build the standard -O2 pipeline and run it.
  ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(level);
  MPM.run(M, MAM);
}
```

This is exactly what AOT and JIT drivers call before codegen. The `OptimizationLevel` enum
spans `O0, O1, O2, O3, Os, Oz`.

---

## 5. Building a *custom* pipeline

Sometimes you want a specific, minimal pipeline — e.g., a frontend that just needs `mem2reg`
+ a few cleanups (common in a JIT for fast compiles):

```cpp
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/SimplifyCFG.h"
#include "llvm/Transforms/Scalar/Reassociate.h"

FunctionPassManager FPM;
FPM.addPass(PromotePass());          // mem2reg
FPM.addPass(InstCombinePass());
FPM.addPass(ReassociatePass());
FPM.addPass(GVNPass());
FPM.addPass(SimplifyCFGPass());

// Adapt the function pipeline to run over a whole module:
ModulePassManager MPM;
MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
MPM.run(M, MAM);
```

You can also parse a textual pipeline (handy for experimentation, mirrors `opt`):

```cpp
PB.parsePassPipeline(MPM, "function(mem2reg,instcombine,gvn,simplifycfg)");
```

The CLI equivalent — invaluable for figuring out *which* passes do what:

```bash
>>> opt -passes='mem2reg,instcombine,gvn,simplifycfg' -S in.ll -o out.ll
>>> opt -print-after-all -passes='mem2reg' -S in.ll -o /dev/null   # watch IR change
>>> opt --print-passes                                              # list every pass name
```

---

## 6. Writing your own pass (new PM)

A new-PM pass is just a struct with a `run` method and a result type. Here's a transform pass
that replaces `x * 2` with `x + x` (silly but illustrative of the rewriting APIs from the
IRBuilder chapter).

```cpp
#include "llvm/IR/PassManager.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
using namespace llvm;

struct MulToAddPass : PassInfoMixin<MulToAddPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    bool changed = false;
    for (BasicBlock &BB : F) {
      // Collect first; we'll erase while iterating, so don't mutate during the loop.
      SmallVector<BinaryOperator *, 8> Worklist;
      for (Instruction &I : BB)
        if (auto *BO = dyn_cast<BinaryOperator>(&I))
          if (BO->getOpcode() == Instruction::Mul)
            if (auto *C = dyn_cast<ConstantInt>(BO->getOperand(1)))
              if (C->equalsInt(2))
                Worklist.push_back(BO);

      for (BinaryOperator *BO : Worklist) {
        IRBuilder<> B(BO);                              // insert before BO
        Value *X = BO->getOperand(0);
        Value *Sum = B.CreateAdd(X, X, "mul2add");
        BO->replaceAllUsesWith(Sum);                    // redirect users
        BO->eraseFromParent();                          // delete the old mul
        changed = true;
      }
    }
    // Tell the manager what we preserved. If we changed the CFG we'd return none().
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
};
```

```
   For each:   %t = mul i32 %x, 2
   We do:      %mul2add = add i32 %x, %x
               replaceAllUsesWith(%t → %mul2add)
               erase %t
   Result:     %mul2add = add i32 %x, %x   (every former user of %t now uses this)
```

### Plugging it in two ways

**(a) Directly in your compiler:**

```cpp
FunctionPassManager FPM;
FPM.addPass(MulToAddPass());
MPM.addPass(createModuleToFunctionPassAdaptor(std::move(FPM)));
```

**(b) As a loadable plugin for `opt`** — register a callback so `opt -passes=mul-to-add`
finds it:

```cpp
extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "MulToAdd", "v1", [](PassBuilder &PB) {
    PB.registerPipelineParsingCallback(
        [](StringRef Name, FunctionPassManager &FPM,
           ArrayRef<PassBuilder::PipelineElement>) {
          if (Name == "mul-to-add") { FPM.addPass(MulToAddPass()); return true; }
          return false;
        });
  }};
}
```

Build as a shared lib and run:

```bash
>>> opt -load-pass-plugin=./MulToAdd.so -passes=mul-to-add -S in.ll -o out.ll
```

---

## 7. Analysis passes and `PreservedAnalyses`

Transforms invalidate analyses. The new PM tracks this precisely so cached results stay
correct.

```
   You ran InstCombine (changed instructions but not the CFG):
     return PreservedAnalyses::none();   // conservative: drop everything
   ─ or, if you know you kept the CFG intact ─
     PreservedAnalyses PA;
     PA.preserveSet<CFGAnalyses>();      // keep dominator tree, loop info, etc.
     return PA;
```

To *consume* an analysis in your pass:

```cpp
PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
  DominatorTree &DT = FAM.getResult<DominatorTreeAnalysis>(F);  // cached or computed
  LoopInfo     &LI = FAM.getResult<LoopAnalysis>(F);
  // ...use DT, LI...
}
```

```
   AnalysisManager caches results keyed by (analysis, IRunit).
   getResult<T>(unit):  return cached  OR  compute, cache, return.
   A transform's PreservedAnalyses tells the manager which cached
   results survive — the rest are invalidated and recomputed on next demand.
```

This explicit invalidation model is the headline improvement over the legacy PM, which
recomputed analyses pessimistically.

---

## Mental model checkpoint

1. Distinguish an *analysis* pass from a *transformation* pass.
2. Name the four pass scopes and give an example pass for each.
3. What does `mem2reg` do, and why is it the linchpin for frontend codegen?
4. In the new PM, what are MAM/FAM and `PassBuilder` responsible for?
5. Write (from memory) the call that builds and runs the standard `-O2` pipeline.
6. In a transform pass, when do you return `PreservedAnalyses::all()` vs `none()`?
7. What is the rewriting idiom: collect → build replacement → RAUW → erase, and why collect
   first?

Next → [03-pass-pipeline.md](03-pass-pipeline.md)

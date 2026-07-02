# 05.03 · Rewrites, Lowering & Dialect Conversion

> The whole point of MLIR is to *transform* and *progressively lower* IR. This chapter covers
> the rewrite engine (pattern-based transformation), canonicalization, and the **dialect
> conversion** framework — the machinery that turns high-level dialects into LLVM-dialect IR
> step by step, with *legality* tracking so you can't accidentally stop halfway.

---

## 1. Two kinds of transformation

```
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ (A) WITHIN-dialect rewrites: simplify/canonicalize ops, fuse, fold.      │
  │     "x + 0 → x", "fuse two linalg.generic into one". Optional, optimize. │
  ├──────────────────────────────────────────────────────────────────────────┤
  │ (B) CROSS-dialect CONVERSION (lowering): replace ops of dialect X with   │
  │     ops of a lower dialect Y, until target dialect is reached. This is   │
  │     the staircase from ch.05.01. Has LEGALITY rules to ensure completion.│
  └──────────────────────────────────────────────────────────────────────────┘
```

Both are expressed with **rewrite patterns**, but conversion adds a *target legality* concept
that drives lowering to a fixed point.

---

## 2. Rewrite patterns — the unit of transformation

A pattern matches some op(s) and replaces them with others. The base mechanism:

```cpp
#include "mlir/IR/PatternMatch.h"
using namespace mlir;

// Rewrite: arith.addf %x, 0.0  ->  %x   (additive identity)
struct AddZeroFold : public OpRewritePattern<arith::AddFOp> {
  using OpRewritePattern::OpRewritePattern;

  LogicalResult matchAndRewrite(arith::AddFOp op,
                                PatternRewriter &rewriter) const override {
    // Is the RHS a constant 0.0?
    auto cst = op.getRhs().getDefiningOp<arith::ConstantOp>();
    if (!cst) return failure();
    auto fattr = dyn_cast<FloatAttr>(cst.getValue());
    if (!fattr || !fattr.getValue().isZero()) return failure();

    rewriter.replaceOp(op, op.getLhs());   // replace the addf with its LHS
    return success();
  }
};
```

```
   matchAndRewrite contract:
     return failure()  → "this pattern doesn't apply here" (try others)
     return success()  → "I matched and rewrote" (via the rewriter)
   ALWAYS mutate IR through the PatternRewriter (replaceOp, eraseOp,
   create<...>, replaceAllUsesWith) — NEVER edit ops directly during a
   rewrite, or the driver's bookkeeping (worklist, undo) breaks.
```

```
   The rewrite DRIVER applies patterns repeatedly to a fixed point:
     worklist ← all ops
     while worklist not empty:
        op ← pop; try each applicable pattern;
        if one succeeds, push affected/neighbor ops back (they may now match)
     stop when no pattern applies anywhere.
```

You collect patterns into a set and run them:

```cpp
RewritePatternSet patterns(&ctx);
patterns.add<AddZeroFold>(&ctx);
// optionally also: dialect/op canonicalization patterns
(void)applyPatternsAndFoldGreedily(module, std::move(patterns));
```

---

## 3. Canonicalization and folding

MLIR builds two simplification mechanisms into ops themselves:

```
   • FOLD: an op can implement `fold()` to compute a constant result or return
     an existing value, with NO new ops. e.g. arith.addi %x, 0 folds to %x.
   • CANONICALIZE: ops register canonicalization PATTERNS to put themselves
     in a normal form (e.g. move constants to the right, collapse chains).

   The `-canonicalize` pass runs all ops' folders + canonicalization patterns.
```

```
   ┌──────────────────────────────────────────────────────────────────────────┐
   │ Declaring `hasCanonicalizer`/`hasFolder` on your op (ch.05.04) means     │
   │ the standard -canonicalize and -cse passes improve YOUR dialect for      │
   │ free — the same "declare properties, inherit transforms" theme as traits.│
   └──────────────────────────────────────────────────────────────────────────┘
```

CLI to apply:

```bash
>>> mlir-opt input.mlir -canonicalize -cse -o out.mlir
```

---

## 4. Dialect Conversion — the lowering framework

Lowering needs more than "apply patterns until stuck." It needs a notion of **what's
allowed in the output** so the process drives toward a target and *fails loudly* if it can't
get there. That's the **DialectConversion** framework, built on three pieces:

```
   ┌──────────────────────┬─────────────────────────────────────────────────────┐
   │ ConversionTarget     │ declares which ops/dialects are LEGAL (allowed to   │
   │                      │ remain) vs ILLEGAL (must be converted away).        │
   ├──────────────────────┼─────────────────────────────────────────────────────┤
   │ ConversionPatterns   │ how to rewrite illegal ops into legal ones          │
   │ (OpConversionPattern)│ (like rewrite patterns, but type-aware).            │
   ├──────────────────────┼─────────────────────────────────────────────────────┤
   │ TypeConverter        │ how to convert TYPES (e.g. memref<?xf32> → an       │
   │                      │ llvm.struct describing ptr+sizes+strides).          │
   └──────────────────────┴─────────────────────────────────────────────────────┘
```

```
   The conversion driver's job:
     "Make every op legal. For each illegal op, find a conversion pattern that
      rewrites it (converting operand/result TYPES via the TypeConverter).
      Repeat until ALL ops are legal. If some illegal op has no pattern →
      ERROR (conversion failed) — you can't silently end up half-lowered."
```

### Two conversion modes

```
   PARTIAL conversion (applyPartialConversion):
     some illegal ops MAY remain if unconverted; useful for incremental lowering
     where another pass finishes the job.

   FULL conversion (applyFullConversion):
     EVERY op must end up legal, or the whole conversion fails. Used for the
     final "everything must be llvm dialect now" step.
```

### Example: declaring a target that wants only the LLVM dialect

```cpp
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Transforms/DialectConversion.h"
using namespace mlir;

ConversionTarget target(ctx);
target.addLegalDialect<LLVM::LLVMDialect>();   // llvm ops are the goal: legal
target.addIllegalDialect<arith::ArithDialect, // these MUST be converted away
                         func::FuncDialect,
                         scf::SCFDialect>();

LLVMTypeConverter typeConverter(&ctx);         // knows memref→struct, index→i64, ...

RewritePatternSet patterns(&ctx);
// populate with the standard lowering patterns for each dialect:
arith::populateArithToLLVMConversionPatterns(typeConverter, patterns);
populateFuncToLLVMConversionPatterns(typeConverter, patterns);
populateSCFToControlFlowConversionPatterns(patterns);          // scf → cf first
cf::populateControlFlowToLLVMConversionPatterns(typeConverter, patterns);
// ... finalize memref, etc.

if (failed(applyFullConversion(module, target, std::move(patterns))))
  signalPassFailure();   // some op couldn't be lowered → real error
```

```
   ┌──────────────────────────────────────────────────────────────────────────┐
   │ The genius of ConversionTarget: you declare the DESTINATION (legal set)  │
   │ and supply the EDGES (patterns). The framework finds the path and        │
   │ guarantees you ARRIVE (full) or tells you exactly which op blocked you.  │
   │ No more "oops, I forgot to lower scf.for and it crashed in translation." │
   └──────────────────────────────────────────────────────────────────────────┘
```

---

## 5. A worked progressive-lowering pipeline

Lowering a typical `scf + arith + memref` module to the LLVM dialect is done as a *sequence*
of conversion passes, each closing one abstraction gap:

```
   Linalg/Tensor (high)
      │ -linalg-bufferize / -one-shot-bufferize   tensors → memrefs
      ▼
   scf + arith + memref
      │ -convert-linalg-to-loops (if any)         structured → scf loops
      │ -convert-scf-to-cf                          scf.for/if → cf.br/cond_br
      ▼
   cf + arith + memref
      │ -convert-arith-to-llvm                      scalar math → llvm dialect
      │ -finalize-memref-to-llvm                    memref → llvm ptr/struct
      │ -convert-func-to-llvm                        func → llvm.func
      │ -convert-cf-to-llvm                          branches → llvm.br
      ▼
   llvm dialect ONLY  ──▶ translate to LLVM IR (ch.05.05)
```

On the CLI (the exact recipe you'll actually run, order matters):

```bash
>>> mlir-opt input.mlir \
      -convert-scf-to-cf \
      -convert-arith-to-llvm \
      -convert-func-to-llvm \
      -finalize-memref-to-llvm \
      -convert-cf-to-llvm \
      -reconcile-unrealized-casts \
      -o lowered.mlir
```

```
   -reconcile-unrealized-casts: during multi-step conversion, MLIR inserts
   temporary `unrealized_conversion_cast` ops where types don't yet line up
   between half-converted regions. This final pass removes the ones that
   cancel out. If some remain, your lowering is incomplete — a useful signal.
```

---

## 6. Type conversion in depth (the memref example)

LLVM has no notion of "a 2-D buffer with strides." So the `TypeConverter` lowers a `memref`
to an **LLVM struct** capturing everything LLVM needs:

```
   memref<?x?xf32>   ──TypeConverter──▶   llvm.struct<(
                                            ptr,        ; allocated pointer
                                            ptr,        ; aligned pointer
                                            i64,        ; offset
                                            array<2 x i64>,  ; sizes [d0, d1]
                                            array<2 x i64>)> ; strides
   (this layout is the "MemRef descriptor" — a runtime record of the buffer)
```

```
   ┌──────────────────────────────────────────────────────────────────────────┐
   │ Lowering isn't just ops — it's TYPES too. A high-level type (memref,     │
   │ tensor, your custom type) becomes a low-level representation (struct of  │
   │ pointers/ints). The TypeConverter centralizes this so every conversion   │
   │ pattern agrees on the representation.                                    │
   └──────────────────────────────────────────────────────────────────────────┘
```

---

## 7. Writing a lowering as a Pass

Conversions are packaged as passes so they slot into the MLIR PassManager (mirrors LLVM's
ch.02):

```cpp
#include "mlir/Pass/Pass.h"
struct MyToLLVMPass : public PassWrapper<MyToLLVMPass, OperationPass<ModuleOp>> {
  void getDependentDialects(DialectRegistry &reg) const override {
    reg.insert<LLVM::LLVMDialect>();           // we PRODUCE llvm dialect
  }
  void runOnOperation() override {
    ModuleOp module = getOperation();
    ConversionTarget target(getContext());
    target.addLegalDialect<LLVM::LLVMDialect>();
    target.addIllegalDialect<arith::ArithDialect, func::FuncDialect>();
    LLVMTypeConverter tc(&getContext());
    RewritePatternSet patterns(&getContext());
    arith::populateArithToLLVMConversionPatterns(tc, patterns);
    populateFuncToLLVMConversionPatterns(tc, patterns);
    if (failed(applyFullConversion(module, target, std::move(patterns))))
      signalPassFailure();
  }
};
```

Run via a `PassManager`:

```cpp
PassManager pm(&ctx);
pm.addPass(createConvertSCFToCFPass());
pm.addPass(std::make_unique<MyToLLVMPass>());
if (failed(pm.run(module))) { /* handle */ }
```

```
   MLIR PassManager ↔ LLVM PassManager (ch.02.02): same idea — a pipeline of
   passes over an IR unit, with analysis caching and nesting (a ModuleOp pass
   can contain function-level pass managers).
```

---

## Mental model checkpoint

1. Distinguish within-dialect rewrites from cross-dialect conversion.
2. What must `matchAndRewrite` return, and why mutate only through the `PatternRewriter`?
3. What does the rewrite driver do to reach a fixed point?
4. Name the three components of the DialectConversion framework and each one's job.
5. Partial vs full conversion — when use each?
6. Why does declaring a `ConversionTarget` make lowering robust (vs ad-hoc patterns)?
7. Walk the pass sequence that lowers `scf+arith+memref` to the LLVM dialect.
8. Why does a `memref` become an LLVM struct, and what's in that struct?
9. What is an `unrealized_conversion_cast` and what does reconciling them tell you?

Next → [04-building-a-dialect.md](04-building-a-dialect.md)

# 05.02 · Dialects, Operations, Types, Attributes & Traits

> Now we get concrete about MLIR's building blocks: how to *read* the textual format, the
> built-in dialects you'll actually use, the type/attribute system, and traits/interfaces —
> the mechanisms that make passes generic across dialects. This is the "vocabulary" chapter.

---

## 1. Reading MLIR's generic and custom syntax

Every op has a **generic form** (always valid, fully explicit) and usually a **custom form**
(pretty, defined by the op). Learn to read both — the generic form is your fallback when
custom syntax confuses you.

```
   GENERIC form (works for ANY op):
   %0 = "arith.addi"(%a, %b) : (i32, i32) -> i32
        ──────────── ──────   ──────────    ───
        op name      operands  operand types result type

   CUSTOM form (nicer, op-specific):
   %0 = arith.addi %a, %b : i32

   Both mean the same thing. mlir-opt --mlir-print-op-generic shows generic form.
```

A full module, top to bottom:

```mlir
module {
  func.func @sum(%a: f32, %b: f32) -> f32 {
    %0 = arith.addf %a, %b : f32
    func.return %0 : f32
  }
}
```

```
   module               ── a container op (has one region)
     func.func @sum     ── a function op (has a body region + a type)
       arith.addf       ── an op from the arith dialect, results %0
       func.return      ── the region's terminator
```

Notice: `module`, `func.func`, even functions are *operations*. It's ops all the way down
(chapter 05.01 §3).

---

## 2. The built-in dialects you will actually use

You rarely invent everything; you compose existing dialects. The ones to know:

```
  ┌───────────┬─────────────────────────────────────────────────────────────┐
  │ builtin   │ the always-present types/attrs: integers, floats, index,    │
  │           │ tensor<>, vector<>, the `module` op, function types.        │
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ func      │ func.func (definitions), func.call, func.return.            │
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ arith     │ scalar arithmetic: addi/addf/muli/mulf/cmpi/cmpf/constant.  │
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ scf       │ structured control flow: scf.for, scf.if, scf.while,        │
  │           │ scf.yield. Loops/ifs as ops-with-regions (NOT raw branches).│
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ cf        │ unstructured control flow: cf.br, cf.cond_br (raw CFG, the  │
  │           │ level just above LLVM's branches).                          │
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ memref    │ buffers/memory: memref.alloc, .load, .store, .dealloc.      │
  │           │ A memref is "a pointer + shape + strides" — addressable mem.│
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ tensor    │ immutable n-d values (SSA tensors); no memory location yet. │
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ vector    │ SIMD: vector<8xf32>, vector.contract, transfer_read/write.  │
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ affine    │ loops/accesses with affine-map constraints→ strong loop opts│
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ linalg    │ structured tensor/buffer ops: linalg.matmul, linalg.generic.│
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ llvm      │ mirrors LLVM IR; the bottom rung before translation.        │
  ├───────────┼─────────────────────────────────────────────────────────────┤
  │ gpu/spirv │ GPU kernels & SPIR-V; nvvm/rocdl for vendor intrinsics.     │
  └───────────┴─────────────────────────────────────────────────────────────┘
```

A taste of `scf` + `arith` + `memref` working together (a sum-reduction):

```mlir
func.func @reduce(%A: memref<?xf32>, %n: index) -> f32 {
  %c0 = arith.constant 0 : index
  %c1 = arith.constant 1 : index
  %init = arith.constant 0.0 : f32
  %sum = scf.for %i = %c0 to %n step %c1 iter_args(%acc = %init) -> f32 {
    %v = memref.load %A[%i] : memref<?xf32>
    %new = arith.addf %acc, %v : f32
    scf.yield %new : f32
  }
  func.return %sum : f32
}
```

```
   Note how scf.for carries loop state via iter_args / yield (SSA loop-carried
   values — the MLIR analog of loop PHIs from ch.02.01's makeSumTo). The loop
   body is a REGION; the whole loop is ONE op a pass can manipulate as a unit.
```

---

## 3. The type system

Types are dialect-extensible, but a rich set is built in:

```
   SCALAR:   i1 i8 i32 i64 (signless, like LLVM)   index (target-width int for
                                                    loop bounds/indices)
             f16 bf16 f32 f64
   SHAPED:   tensor<4x8xf32>     ranked tensor (value semantics, immutable)
             tensor<?x?xf32>     dynamic dims with '?'
             tensor<*xf32>       unranked
             vector<8xf32>       SIMD vector (fixed length)
             memref<4x8xf32>     buffer in memory (reference semantics)
             memref<?xf32, strided<...>>   with layout/strides/address space
   FUNCTION: (f32, f32) -> f32
   plus dialect-defined types (e.g. !llvm.ptr, !llvm.struct<...>, your own !toy.struct)
```

```
   tensor vs memref — a critical distinction:
   ┌──────────────────────────────────────────────────────────────────────────┐
   │ tensor<...>  : an SSA VALUE. Immutable, no address. "the data."          │
   │ memref<...>  : a BUFFER reference. Has a location, you load/store to it. │
   │ "Bufferization" is the lowering that turns tensors (values) into memrefs │
   │ (memory) — a key step toward LLVM, which only knows memory & pointers.   │
   └──────────────────────────────────────────────────────────────────────────┘
```

Custom types are spelled with a leading `!`: `!llvm.ptr`, `!toy.matrix<2x3xf64>`.

---

## 4. Attributes — compile-time constant data

Attributes are the op's *static* metadata (chapter 05.01 §3): not SSA values, but constants
baked into the op.

```
   %c = arith.constant 42 : i32
                       ──
                       this 42 is an ATTRIBUTE (IntegerAttr), not an operand.

   linalg ops carry indexing_maps, iterator_types as ARRAY/AFFINE-MAP attributes.
   func.func carries its name (@sum) and function type as attributes.
```

```
   Common attribute kinds:
     IntegerAttr, FloatAttr            42, 3.14
     StringAttr                        "hello"
     ArrayAttr                         [1, 2, 3]
     DictionaryAttr                    {fastmath = ..., name = ...}
     TypeAttr                          a type used as data
     AffineMapAttr                     (d0,d1)->(d1,d0)  (a transpose map)
     DenseElementsAttr                 a whole constant tensor's contents
   Spelled with a leading '#' for custom: #toy.fancy<...>
```

Attributes are *uniqued* (interned) in the `MLIRContext`, like LLVM constants.

---

## 5. Traits — declarative op properties

A **trait** is a reusable property you attach to an op; the framework and passes then treat
the op generically. Traits encode invariants and enable optimizations without per-op code.

```
  ┌──────────────────────────┬──────────────────────────────────────────────────┐
  │ Pure / NoMemoryEffect    │ op has no side effects → dead-code elimination,  │
  │ (MemoryEffectsTrait)     │ CSE, hoisting all work automatically.            │
  │ Commutative              │ a+b == b+a → canonicalization can reorder.       │
  │ SameOperandsAndResultType│ verifier checks all operands/results share type. │
  │ Terminator               │ op must end a block (like LLVM terminators).     │
  │ SingleBlock /            │ region-structure constraints on container ops.   │
  │ SingleBlockImplicitTerm. │                                                  │
  │ IsolatedFromAbove        │ region can't reference SSA values from outside   │
  │                          │ (e.g. func bodies) → enables parallel passes.    │
  └──────────────────────────┴──────────────────────────────────────────────────┘
```

```
   Why traits matter: a single generic pass like CSE or DCE works on YOUR
   custom op for free IF your op declares the right traits (e.g. Pure). You
   don't write op-specific optimization code — you DECLARE properties and
   inherit the optimizations. This is MLIR's "generic transforms" superpower.
```

---

## 6. Interfaces — generic behavior across dialects

Where traits are static flags, **interfaces** are like C++ virtual methods an op implements,
letting a pass call into ops it doesn't know about.

```
   A pass wants to "lower any op to LLVM" but can't know every op. Solution:
   ops implement an INTERFACE; the pass calls the interface method.

   Examples:
     LoopLikeOpInterface     "what's your induction var / bounds?" → LICM works
                              on scf.for, affine.for, your custom loop, uniformly.
     MemoryEffectOpInterface  "what memory do you read/write?" → alias/DCE.
     CallOpInterface          "who do you call?" → inlining, call graph.
     LLVMTranslationInterface "translate yourself to LLVM IR."
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ Traits   = declarative PROPERTIES (compile-time flags) → free generic   │
   │            transforms (DCE, CSE, canonicalization).                     │
   │ Interfaces = behavioral CONTRACTS (methods) → passes operate on unknown │
   │            ops by asking them questions. Together they're how ONE pass  │
   │            serves MANY dialects without knowing them in advance.        │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 7. The MLIRContext, ops, and the C++ object model

Just as LLVM has `LLVMContext`/`Module`/`Instruction`, MLIR has:

```
   MLIRContext      owns dialects, uniqued types & attributes (like LLVMContext)
   ModuleOp         the top-level container (an op with one region)
   Operation*       the generic op object; everything is one
   OpBuilder        the builder/cursor (MLIR's IRBuilder analog) for creating ops
   Value            an SSA value (an op result or block argument)
   Type / Attribute uniqued, immutable, value-typed handles
   Block / Region   the nesting structure
```

Creating IR in C++ (the analog of chapter 01.03's codegen, but for MLIR):

```cpp
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
using namespace mlir;

MLIRContext ctx;
ctx.loadDialect<func::FuncDialect, arith::ArithDialect>();
OpBuilder b(&ctx);

// Build: func @add(%a: f32, %b: f32) -> f32 { return %a + %b }
auto loc = b.getUnknownLoc();
auto f32 = b.getF32Type();
auto fnType = b.getFunctionType({f32, f32}, {f32});
auto module = ModuleOp::create(loc);
b.setInsertionPointToEnd(module.getBody());

auto fn = b.create<func::FuncOp>(loc, "add", fnType);
Block *entry = fn.addEntryBlock();
b.setInsertionPointToStart(entry);
Value a = entry->getArgument(0), bb = entry->getArgument(1);
Value sum = b.create<arith::AddFOp>(loc, a, bb);   // %sum = arith.addf %a, %b
b.create<func::ReturnOp>(loc, sum);
module.print(llvm::outs());
```

```
   Side-by-side with LLVM codegen (ch.01.03):
     LLVMContext  ↔ MLIRContext
     IRBuilder    ↔ OpBuilder
     CreateFAdd   ↔ b.create<arith::AddFOp>
     Function     ↔ func::FuncOp
     BasicBlock   ↔ Block (but inside a Region, possibly nested)
     every Value is typed in both.
   If you internalized ch.01.03, MLIR codegen is the same moves with ops.
```

> Note `b.create<arith::AddFOp>` — you create *typed C++ op classes*. These classes are
> generated from TableGen definitions (chapter 05.04). That's the bridge from "reading MLIR"
> to "defining your own dialect."

---

## Mental model checkpoint

1. What's the difference between an op's generic and custom syntax, and how do you print
   generic form?
2. Distinguish `tensor` from `memref`, and name the lowering that connects them.
3. What is `index` and where is it used?
4. Are operands and attributes both SSA values? What's the distinction?
5. Give two traits and the optimization each one enables for free.
6. How do interfaces let one pass operate on dialects it's never heard of?
7. Map MLIR's `MLIRContext`/`OpBuilder`/`func::FuncOp` to their LLVM analogs.
8. Why do you write `b.create<arith::AddFOp>` with a concrete C++ class, and where does that
   class come from?

Next → [03-lowering-and-conversion.md](03-lowering-and-conversion.md)

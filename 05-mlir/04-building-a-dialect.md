# 05.04 · Building Your Own Dialect (ODS / TableGen)

> To use MLIR for *your* language or domain, you define a dialect: your own ops, types, and
> attributes. You *could* write these in C++ by hand, but MLIR provides **ODS** (Operation
> Definition Specification) in **TableGen** — a declarative DSL that generates the C++ for you.
> We build a small `toy` dialect end to end.

---

## 1. Why TableGen / ODS

Hand-writing an op in C++ means writing: a class, a builder, the verifier, the parser, the
printer, accessors, trait wiring — ~150 lines of boilerplate *per op*. ODS lets you *declare*
the op in ~10 lines; `mlir-tblgen` generates all that C++.

```
   YOU WRITE (TableGen .td):                 mlir-tblgen GENERATES (C++ .h.inc/.cpp.inc):
   ──────────────────────────                ──────────────────────────────────────────
   def AddOp : Toy_Op<"add", [Pure]> {       class AddOp : public Op<AddOp, ...> {
     let arguments = (ins F64Tensor:$lhs,       // accessors getLhs()/getRhs()
                          F64Tensor:$rhs);       // builders build(...)
     let results   = (outs F64Tensor:$res);      // verifier verifyInvariants()
   }                                            // parser/printer
                                              };  // trait wiring (Pure → DCE/CSE)
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ ODS is to MLIR ops what a schema is to a database: a single declarative │
   │ source of truth from which the framework generates correct, consistent  │
   │ C++. You edit the .td; the build regenerates the boilerplate.           │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Declaring the dialect

First, the dialect itself — its namespace and a couple of registration knobs:

```tablegen
// ToyDialect.td
include "mlir/IR/OpBase.td"
include "mlir/Interfaces/SideEffectInterfaces.td"

def Toy_Dialect : Dialect {
  let name = "toy";                       // ops will be "toy.add", etc.
  let cppNamespace = "::toy";             // generated C++ lives in namespace toy
  let summary = "A tiny tutorial dialect.";
  let useDefaultTypePrinterParser = 1;    // auto syntax for custom types
}

// A base class so every Toy op shares the dialect + the Op template.
class Toy_Op<string mnemonic, list<Trait> traits = []>
    : Op<Toy_Dialect, mnemonic, traits>;
```

```
   Dialect.name = "toy"  →  every op is namespaced "toy.<mnemonic>" in the IR.
   cppNamespace          →  generated classes: ::toy::AddOp, ::toy::ConstantOp...
```

---

## 3. Defining operations

A constant op (carries an attribute, produces a tensor) and an add op (two tensors → tensor):

```tablegen
// ToyOps.td
include "ToyDialect.td"
include "mlir/Interfaces/InferTypeOpInterface.td"

// toy.constant: a tensor literal. Result computed from an attribute.
def ConstantOp : Toy_Op<"constant", [Pure]> {
  let summary = "constant tensor";
  let arguments = (ins F64ElementsAttr:$value);   // the constant data (attribute)
  let results   = (outs F64Tensor:$result);
  // Custom assembly: `toy.constant dense<...> : tensor<...>`
  let assemblyFormat = "$value attr-dict `:` type($result)";
  let hasVerifier = 1;                            // we'll write a C++ verifier
}

// toy.add: elementwise add of two f64 tensors.
def AddOp : Toy_Op<"add", [Pure]> {
  let summary = "elementwise tensor add";
  let arguments = (ins F64Tensor:$lhs, F64Tensor:$rhs);
  let results   = (outs F64Tensor:$result);
  let assemblyFormat = "$lhs `,` $rhs attr-dict `:` type($result)";
  // Convenience builder: result type inferred from lhs.
  let builders = [
    OpBuilder<(ins "mlir::Value":$lhs, "mlir::Value":$rhs), [{
      build($_builder, $_state, lhs.getType(), lhs, rhs);
    }]>
  ];
}

// toy.print: a side-effecting op (NOT Pure) — prints a tensor.
def PrintOp : Toy_Op<"print"> {
  let summary = "print a tensor";
  let arguments = (ins F64Tensor:$input);
  let assemblyFormat = "$input attr-dict `:` type($input)";
}
```

Each ODS field maps to a part of the op anatomy from chapter 05.01 §3:

```
   arguments (ins ...)   → operands (Value) AND attributes (compile-time)
   results   (outs ...)  → result values
   traits [Pure, ...]    → declarative properties (free DCE/CSE for Pure ops)
   assemblyFormat        → the custom textual syntax (auto parser+printer!)
   builders              → convenience C++ constructors
   hasVerifier           → you provide verify() in C++ for extra invariants
```

```
   The resulting IR you can now write/parse:
   ─────────────────────────────────────────────────────────────
   %0 = toy.constant dense<[1.0, 2.0, 3.0]> : tensor<3xf64>
   %1 = toy.add %0, %0 : tensor<3xf64>
   toy.print %1 : tensor<3xf64>
```

---

## 4. The verifier — declaring op invariants

`hasVerifier = 1` means you write a C++ method enforcing constraints the type system can't.
For `ConstantOp`, check that the attribute's shape matches the result type:

```cpp
// ToyOps.cpp
llvm::LogicalResult toy::ConstantOp::verify() {
  auto attrType = llvm::cast<mlir::RankedTensorType>(getValue().getType());
  auto resType  = llvm::cast<mlir::RankedTensorType>(getResult().getType());
  if (attrType.getShape() != resType.getShape())
    return emitOpError("attribute shape ")
           << attrType.getShape() << " != result shape " << resType.getShape();
  return llvm::success();
}
```

```
   The verifier runs automatically (mlir-opt -verify-each, pass boundaries).
   It's your op's contract — malformed toy.constant is rejected with a good
   diagnostic, exactly like LLVM's verifier rejects malformed IR (ch.00.03).
```

---

## 5. Registering and the C++ glue

A little non-generated C++ ties it together (these `.inc` files are produced by
`mlir-tblgen`):

```cpp
// ToyDialect.h
#include "mlir/IR/Dialect.h"
#include "mlir/IR/OpDefinition.h"
#include "ToyOpsDialect.h.inc"          // generated: dialect class decl
#define GET_OP_CLASSES
#include "ToyOps.h.inc"                  // generated: op class decls (AddOp, ...)

// ToyDialect.cpp
#include "ToyDialect.h"
#include "ToyOpsDialect.cpp.inc"         // generated: dialect definitions
void toy::ToyDialect::initialize() {
  addOperations<
#define GET_OP_LIST
#include "ToyOps.cpp.inc"                // generated: register all ops
  >();
}
#define GET_OP_CLASSES
#include "ToyOps.cpp.inc"                // generated: op method definitions
```

Load it in a tool/driver:

```cpp
mlir::MLIRContext ctx;
ctx.getOrLoadDialect<toy::ToyDialect>();
// now you can parse/build toy.* ops
```

---

## 6. The build wiring (CMake + mlir-tblgen)

CMake invokes `mlir-tblgen` to turn `.td` into `.inc`, then compiles your dialect lib:

```cmake
# CMakeLists.txt (dialect library)
find_package(MLIR REQUIRED CONFIG)
list(APPEND CMAKE_MODULE_PATH "${MLIR_CMAKE_DIR}" "${LLVM_CMAKE_DIR}")
include(TableGen)
include(AddLLVM)
include(AddMLIR)

set(LLVM_TARGET_DEFINITIONS ToyOps.td)
mlir_tablegen(ToyOps.h.inc -gen-op-decls)            # op class declarations
mlir_tablegen(ToyOps.cpp.inc -gen-op-defs)           # op method definitions
mlir_tablegen(ToyOpsDialect.h.inc -gen-dialect-decls -dialect=toy)
mlir_tablegen(ToyOpsDialect.cpp.inc -gen-dialect-defs -dialect=toy)
add_public_tablegen_target(ToyOpsIncGen)

add_mlir_dialect_library(MLIRToy
  ToyDialect.cpp ToyOps.cpp
  DEPENDS ToyOpsIncGen
  LINK_LIBS PUBLIC MLIRIR MLIRSupport)
```

```
   Build flow:
     ToyOps.td ──[mlir-tblgen -gen-op-defs]──▶ ToyOps.cpp.inc ─┐
     ToyOps.td ──[mlir-tblgen -gen-op-decls]──▶ ToyOps.h.inc ──┼─▶ compiled into
     (dialect defs likewise) ──────────────────────────────────┘   libMLIRToy
   Edit the .td → reconfigure/build → boilerplate regenerated. One source of truth.
```

---

## 7. Custom types and attributes (briefly)

Beyond ops, you can declare custom *types* and *attributes* the same way:

```tablegen
def Toy_StructType : TypeDef<Toy_Dialect, "Struct"> {
  let mnemonic = "struct";
  let parameters = (ins ArrayRefParameter<"mlir::Type">:$elementTypes);
  let assemblyFormat = "`<` $elementTypes `>`";   // !toy.struct<f64, f64>
}
```

```
   Now !toy.struct<f64, f64> is a first-class type in your dialect, with
   generated storage, uniquing, parser/printer. Same ODS leverage as ops.
   (Custom attributes use AttrDef similarly: #toy.myattr<...>.)
```

---

## 8. Adding optimizations to your dialect

Because your op declared traits and (optionally) folders/canonicalizers, the standard passes
work on it. To add domain rewrites (e.g., `transpose(transpose(x)) → x`), register
canonicalization patterns:

```tablegen
def TransposeOp : Toy_Op<"transpose", [Pure]> {
  let arguments = (ins F64Tensor:$input);
  let results   = (outs F64Tensor:$result);
  let hasCanonicalizer = 1;          // we'll add a C++ pattern
}
```

```cpp
// transpose(transpose(x)) -> x
struct SimplifyRedundantTranspose : OpRewritePattern<toy::TransposeOp> {
  using OpRewritePattern::OpRewritePattern;
  LogicalResult matchAndRewrite(toy::TransposeOp op, PatternRewriter &r) const override {
    auto inner = op.getInput().getDefiningOp<toy::TransposeOp>();
    if (!inner) return failure();
    r.replaceOp(op, inner.getInput());   // peel both transposes
    return success();
  }
};
void toy::TransposeOp::getCanonicalizationPatterns(RewritePatternSet &p, MLIRContext *c) {
  p.add<SimplifyRedundantTranspose>(c);
}
```

```
   Now `mlir-opt -canonicalize` simplifies your toy.transpose chains — your
   domain knowledge, expressed once, plugged into MLIR's generic driver.
   This is the same pattern API as ch.05.03 §2, now serving YOUR dialect.
```

---

## Mental model checkpoint

1. Why use ODS/TableGen instead of writing ops in C++ by hand?
2. Map the ODS fields `arguments`, `results`, `traits`, `assemblyFormat` to op anatomy.
3. What does `assemblyFormat` generate for you?
4. What does the `Pure` trait buy your op automatically?
5. What goes in a `verify()` method vs what the type system already checks?
6. What does `mlir-tblgen` produce from a `.td`, and how does CMake wire it in?
7. How do you give your dialect custom types, and what syntax marks them in IR?
8. How do you teach the standard `-canonicalize` pass a domain rewrite for your op?

Next → [05-mlir-to-llvm.md](05-mlir-to-llvm.md)

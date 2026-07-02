# 02.01 · The IRBuilder API in Depth

> `IRBuilder` is the single most-used class when generating IR. This chapter is a reference
> *and* a tutorial: how the builder works, every category of instruction you'll create, how
> to manipulate the CFG, types and constants, and the inspection/use-def APIs that let you
> write transformations.

---

## 1. The builder as a positioned factory

```
   An IRBuilder has TWO jobs:
     (1) remember an INSERTION POINT  (a basic block + position within it)
     (2) CREATE instructions there and return their Value*

          ┌──────────── BasicBlock "entry" ──────────────┐
          │  %a = ...                                    │
          │  %b = ...                                    │
          │           ◀── insertion point (cursor)       │
          └──────────────────────────────────────────────┘
   Builder.CreateAdd(x,y)  inserts here, pushing the cursor down.
```

Positioning the cursor:

```cpp
IRBuilder<> B(Ctx);
B.SetInsertPoint(BB);              // at the END of block BB
B.SetInsertPoint(&BB, BB.begin()); // before the first instruction of BB
B.SetInsertPoint(SomeInstruction); // immediately BEFORE that instruction
auto saved = B.saveIP();           // save current insertion point
B.restoreIP(saved);                // restore it later
```

> Saving/restoring the insertion point matters when, e.g., you must add an `alloca` to the
> entry block while in the middle of emitting code elsewhere (the mutable-variable pattern).

The template parameters (`IRBuilder<>`) let you plug in a *constant folder* and *inserter*.
The default `IRBuilder<>` folds constants as you build:

```cpp
B.CreateAdd(B.getInt32(2), B.getInt32(3));  // returns the constant `i32 5`, no instruction!
```

This free constant folding is why naive frontend code already produces decent IR.

---

## 2. Types — how to spell them

Types are uniqued in the `LLVMContext`; you ask the context (or the builder) for them.

```cpp
// Via the builder (convenient):
B.getInt1Ty(); B.getInt8Ty(); B.getInt32Ty(); B.getInt64Ty();
B.getFloatTy(); B.getDoubleTy(); B.getVoidTy();
B.getPtrTy();                       // opaque pointer (optionally an address space)

// Via Type:: static methods:
llvm::Type::getInt32Ty(Ctx);
llvm::Type::getDoubleTy(Ctx);

// Derived types:
auto *ArrTy  = llvm::ArrayType::get(B.getInt32Ty(), 10);          // [10 x i32]
auto *VecTy  = llvm::VectorType::get(B.getFloatTy(), 4, false);   // <4 x float>
auto *StrTy  = llvm::StructType::get(Ctx, {B.getInt32Ty(),        // {i32, ptr}
                                           B.getPtrTy()});
auto *FnTy   = llvm::FunctionType::get(B.getInt32Ty(),            // i32(i32, ptr)
                                       {B.getInt32Ty(), B.getPtrTy()}, false);
```

```
   Type hierarchy (simplified):
     Type
      ├── IntegerType        i1, i8, i32, iN
      ├── floating types     half, float, double, fp128, ...
      ├── PointerType        ptr  (opaque; addrspace optional)
      ├── FunctionType       ret(params...)
      ├── StructType         {T, T, ...}  named or literal
      ├── ArrayType          [N x T]
      ├── VectorType         <N x T> / <vscale x N x T> (scalable)
      └── VoidType, LabelType, ...
```

---

## 3. Constants

Constants are `Value`s too, and uniqued — `getInt32(5)` always returns the same object.

```cpp
B.getInt32(42);                                   // i32 42
B.getInt1(true);                                  // i1 true
llvm::ConstantInt::get(B.getInt64Ty(), 100);      // i64 100
llvm::ConstantFP::get(Ctx, llvm::APFloat(3.14));  // double 3.14
llvm::ConstantPointerNull::get(B.getPtrTy());     // null ptr
llvm::UndefValue::get(B.getInt32Ty());            // i32 undef
llvm::PoisonValue::get(B.getInt32Ty());           // i32 poison
llvm::ConstantAggregateZero::get(ArrTy);          // zeroinitializer
// String:
B.CreateGlobalStringPtr("hello\n");               // creates a private global, returns ptr
```

`undef` vs `poison`: both mean "no particular value," but `poison` is "more toxic" — it
taints dependent computations and enables more aggressive optimization. Use `poison` for
genuinely-unreachable/uninitialized values in modern IR.

---

## 4. The instruction-creation API, by category

Every `CreateXxx` returns a `Value*` (the result) — usually an `Instruction*` you can keep.

### Arithmetic / bitwise

```cpp
B.CreateAdd(L, R, "t");   B.CreateSub(L, R);   B.CreateMul(L, R);
B.CreateSDiv(L, R);       B.CreateUDiv(L, R);  B.CreateSRem(L, R);
B.CreateFAdd(L, R);       B.CreateFSub(L, R);  B.CreateFMul(L, R);  B.CreateFDiv(L, R);
B.CreateAnd(L, R);        B.CreateOr(L, R);    B.CreateXor(L, R);
B.CreateShl(L, R);        B.CreateLShr(L, R);  B.CreateAShr(L, R);
B.CreateNeg(V);           B.CreateNot(V);
// With wrap flags:
B.CreateNSWAdd(L, R);     B.CreateNUWAdd(L, R);
```

### Memory

```cpp
auto *slot = B.CreateAlloca(B.getInt32Ty(), nullptr, "x");  // stack slot, returns ptr
B.CreateStore(B.getInt32(7), slot);                         // store i32 7, ptr %x
auto *v = B.CreateLoad(B.getInt32Ty(), slot, "v");          // %v = load i32, ptr %x
// GEP: address arithmetic
auto *ep = B.CreateGEP(ArrTy, base, {B.getInt32(0), B.getInt32(3)}, "elem");
auto *ip = B.CreateInBoundsGEP(ElemTy, base, {B.getInt64(idx)}, "p"); // with inbounds
```

### Comparisons (return i1)

```cpp
B.CreateICmpEQ(L, R);  B.CreateICmpNE(L, R);
B.CreateICmpSLT(L, R); B.CreateICmpSGT(L, R);   // signed
B.CreateICmpULT(L, R); B.CreateICmpUGT(L, R);   // unsigned
B.CreateFCmpOLT(L, R); B.CreateFCmpONE(L, R);   // ordered float compares
```

### Casts / conversions

```cpp
B.CreateZExt(v, B.getInt32Ty());     // zero-extend
B.CreateSExt(v, B.getInt64Ty());     // sign-extend
B.CreateTrunc(v, B.getInt8Ty());     // narrow
B.CreateSIToFP(v, B.getDoubleTy());  // signed int → float
B.CreateFPToSI(v, B.getInt32Ty());   // float → signed int
B.CreateUIToFP(v, B.getDoubleTy());  // unsigned int → float
B.CreateBitCast(v, DstTy);           // reinterpret (same size)
B.CreatePtrToInt(p, B.getInt64Ty());
B.CreateIntToPtr(i, B.getPtrTy());
```

### Control flow (terminators)

```cpp
B.CreateRet(v);                    // ret <v>
B.CreateRetVoid();                 // ret void
B.CreateBr(destBB);                // unconditional branch
B.CreateCondBr(cond, thenBB, elseBB);   // conditional (cond is i1)
auto *sw = B.CreateSwitch(val, defaultBB, /*numCases hint=*/3);
sw->addCase(B.getInt32(0), caseBB0);
B.CreateUnreachable();
```

### Calls, PHIs, select

```cpp
B.CreateCall(callee /*Function* or FunctionCallee*/, {arg0, arg1}, "r");
auto *phi = B.CreatePHI(B.getInt32Ty(), /*numIncoming=*/2, "p");
phi->addIncoming(valA, blockA);
phi->addIncoming(valB, blockB);
B.CreateSelect(cond, vTrue, vFalse, "sel");   // branchless ternary: cond ? vTrue : vFalse
```

### Intrinsics

```cpp
// Call an intrinsic by ID (LLVM provides math, memcpy, overflow-checked arith, etc.)
auto *fn = llvm::Intrinsic::getDeclaration(M, llvm::Intrinsic::sqrt, {B.getDoubleTy()});
B.CreateCall(fn, {x});
// Convenience wrappers exist for common ones:
B.CreateMemCpy(dst, dstAlign, src, srcAlign, sizeVal);
```

---

## 5. Creating functions and blocks

```cpp
auto *FT = llvm::FunctionType::get(B.getInt32Ty(), {B.getInt32Ty()}, false);
auto *F  = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "g", M);
F->getArg(0)->setName("n");

auto *entry = llvm::BasicBlock::Create(Ctx, "entry", F);  // attach to F now
auto *loop  = llvm::BasicBlock::Create(Ctx, "loop");      // detached; attach later:
F->insert(F->end(), loop);                                // append to F
```

```
   Function::Create(type, linkage, name, Module)
        │   creates the Function and adds it to the Module
        ▼
   BasicBlock::Create(ctx, name, Function)
        │   creates a block; if Function given, appends it
        ▼
   Builder.SetInsertPoint(block); Builder.CreateXxx(...)
        │   fill the block with instructions
```

---

## 6. Inspecting IR: use-def chains, iteration, RTTI

To write *transformations* you must read existing IR. The key APIs:

```cpp
// Iterate the structure:
for (Function &F : *M)
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      ... ;

// Operands (def → I) and users (I → uses): the use-def graph.
for (Use &U : I.operands())  Value *operand = U.get();
for (User *U : V->users())   ... ;          // who uses V?
unsigned n = V->getNumUses();

// RTTI: identify instruction kinds with dyn_cast / isa.
if (auto *BO = llvm::dyn_cast<llvm::BinaryOperator>(&I)) {
  if (BO->getOpcode() == llvm::Instruction::Mul) { ... }
}
if (auto *CI = llvm::dyn_cast<llvm::CallInst>(&I)) {
  llvm::Function *callee = CI->getCalledFunction();
}

// Replace and erase (the core rewriting primitives):
I.replaceAllUsesWith(NewValue);   // RAUW: redirect every user of I to NewValue
I.eraseFromParent();              // remove I from its block and delete it
```

The use-def graph is the foundation of every optimization:

```
        %a = mul i32 %x, %x        def: %a
            ╲
             ╲── used by ──▶  %b = add i32 %a, 1     (%a has one user: %b)

   "replaceAllUsesWith(%a, %c)" rewires %b to use %c instead, then %a is dead.
```

> `llvm::dyn_cast<T>(V)` is LLVM's fast custom RTTI (not C++ `dynamic_cast`): returns `T*` if
> `V` is a `T`, else `nullptr`. `isa<T>(V)` returns bool. `cast<T>(V)` asserts. These three
> are everywhere in pass code.

---

## 7. A from-scratch function with a loop (putting it together)

Build `i32 sum_to(i32 n)` returning `0+1+...+n` using a loop and a PHI — no source language,
pure API. This is the canonical "I can drive IRBuilder" exercise.

```cpp
llvm::Function *makeSumTo(llvm::Module *M, llvm::LLVMContext &Ctx) {
  llvm::IRBuilder<> B(Ctx);
  auto *i32 = B.getInt32Ty();
  auto *F = llvm::Function::Create(
      llvm::FunctionType::get(i32, {i32}, false),
      llvm::Function::ExternalLinkage, "sum_to", M);
  auto *n = F->getArg(0); n->setName("n");

  auto *entry = llvm::BasicBlock::Create(Ctx, "entry", F);
  auto *loop  = llvm::BasicBlock::Create(Ctx, "loop", F);
  auto *done  = llvm::BasicBlock::Create(Ctx, "done", F);

  // entry: jump into the loop
  B.SetInsertPoint(entry);
  B.CreateBr(loop);

  // loop: i and acc are PHIs (loop-carried values)
  B.SetInsertPoint(loop);
  auto *i   = B.CreatePHI(i32, 2, "i");
  auto *acc = B.CreatePHI(i32, 2, "acc");
  i->addIncoming(B.getInt32(0), entry);     // i starts at 0
  acc->addIncoming(B.getInt32(0), entry);   // acc starts at 0

  auto *accNext = B.CreateAdd(acc, i, "acc.next");
  auto *iNext   = B.CreateAdd(i, B.getInt32(1), "i.next");
  auto *cond    = B.CreateICmpSGT(iNext, n, "cond");  // iNext > n ?

  i->addIncoming(iNext, loop);              // loop-carried updates
  acc->addIncoming(accNext, loop);
  B.CreateCondBr(cond, done, loop);         // exit when iNext > n

  // done: return acc.next
  B.SetInsertPoint(done);
  B.CreateRet(accNext);
  return F;
}
```

The resulting IR + CFG:

```
   entry:  br label %loop
              │
              ▼
   loop:   %i   = phi [0,entry], [%i.next,  loop]
           %acc = phi [0,entry], [%acc.next,loop]
           %acc.next = add %acc, %i
           %i.next   = add %i, 1
           %cond     = icmp sgt %i.next, %n
           br %cond, done, loop          ◀── back-edge to itself
              │ (cond true)
              ▼
   done:   ret %acc.next
```

Notice the two loop-carried PHIs (`i`, `acc`) with incoming values from *both* `entry` (the
initial values) and `loop` (the updated values). This is the canonical SSA loop shape — every
`for` loop you ever compile looks like this underneath.

---

## Mental model checkpoint

1. What two pieces of state does an `IRBuilder` carry?
2. Why does `B.CreateAdd(getInt32(2), getInt32(3))` not create an instruction?
3. How do you spell `[10 x i32]`, `<4 x float>`, and `i32(i32, ptr)` as types?
4. What's the difference between `undef` and `poison`?
5. Explain `replaceAllUsesWith` and when you'd follow it with `eraseFromParent`.
6. What do `isa<T>`, `dyn_cast<T>`, `cast<T>` each return/do?
7. In `makeSumTo`, why do `i` and `acc` each have two incoming PHI values, and from which
   blocks?

Next → [02-optimization-passes.md](02-optimization-passes.md)

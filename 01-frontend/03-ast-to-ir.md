# 01.03 · Codegen — AST to LLVM IR

> This is where your frontend meets LLVM. We walk the AST and, for each node, emit IR using
> `IRBuilder`. By the end you can turn the entire Toy language into a valid `Module`. This is
> the exact `Module` that AOT, JIT, and (conceptually) MLIR pipelines consume downstream.

---

## 1. The three global objects every codegen needs

```cpp
// Codegen.cpp
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include <map>

static std::unique_ptr<llvm::LLVMContext> TheContext;
static std::unique_ptr<llvm::Module>      TheModule;
static std::unique_ptr<llvm::IRBuilder<>> Builder;
static std::map<std::string, llvm::Value*> NamedValues;   // symbol table for params

static void InitializeModule() {
  TheContext = std::make_unique<llvm::LLVMContext>();
  TheModule  = std::make_unique<llvm::Module>("toy", *TheContext);
  Builder    = std::make_unique<llvm::IRBuilder<>>(*TheContext);
}
```

What each is for:

```
  ┌───────────────┬────────────────────────────────────────────────────────────┐
  │ LLVMContext   │ owns all the uniqued types/constants. One per thread.      │
  │ Module        │ the container we fill with functions & globals.            │
  │ IRBuilder     │ the "cursor" + factory: it inserts new instructions at a   │
  │               │ chosen point and hands back the resulting Value*.          │
  │ NamedValues   │ maps a source variable name → its LLVM Value (here, the    │
  │               │ function argument). Our scope is flat (params only) so a   │
  │               │ single map suffices; real langs use a stack of scopes.     │
  └───────────────┴────────────────────────────────────────────────────────────┘
```

The IRBuilder mental model:

```
   IRBuilder is a typewriter positioned in a basic block.
   SetInsertPoint(BB)  ── move the cursor to the end of block BB
   CreateAdd(a,b)      ── type a new 'add' instruction here, return its Value*
   CreateRet(v)        ── type a 'ret', which also TERMINATES the block
```

---

## 2. Leaf nodes: numbers and variables

```cpp
llvm::Value *NumberExprAST::codegen() {
  // A double literal is a ConstantFP. Constants are uniqued in the context.
  return llvm::ConstantFP::get(*TheContext, llvm::APFloat(Val));
}

llvm::Value *VariableExprAST::codegen() {
  // Look up the name in our symbol table (function params live here).
  llvm::Value *V = NamedValues[Name];
  if (!V) { fprintf(stderr, "Unknown variable name: %s\n", Name.c_str()); return nullptr; }
  return V;
}
```

```
   NumberExprAST(3.14)  ──▶  ConstantFP  ──▶  IR literal: double 3.140000e+00
   VariableExprAST("a") ──▶  NamedValues["a"]  ──▶  the %a argument Value*
```

> In Toy, variables are only function parameters, which are already SSA values, so we can
> store them directly in `NamedValues`. When we add *mutable* locals, we switch to
> alloca-backed storage and `load`/`store` (see §7) so `mem2reg` can rebuild SSA — exactly
> the technique from [../00-foundations/04-ssa-and-cfg.md](../00-foundations/04-ssa-and-cfg.md).

---

## 3. Binary expressions

```cpp
llvm::Value *BinaryExprAST::codegen() {
  llvm::Value *L = LHS->codegen();      // recurse: codegen children first
  llvm::Value *R = RHS->codegen();
  if (!L || !R) return nullptr;

  switch (Op) {
  case '+': return Builder->CreateFAdd(L, R, "addtmp");
  case '-': return Builder->CreateFSub(L, R, "subtmp");
  case '*': return Builder->CreateFMul(L, R, "multmp");
  case '<':
    // fcmp ult yields i1; convert that bool to a double (0.0 or 1.0).
    L = Builder->CreateFCmpULT(L, R, "cmptmp");
    return Builder->CreateUIToFP(L, llvm::Type::getDoubleTy(*TheContext), "booltmp");
  default:
    fprintf(stderr, "invalid binary operator\n"); return nullptr;
  }
}
```

Codegen is **post-order**: emit the operands' IR, then the operator that combines them. For
`a + b`:

```
   BinaryExprAST('+')
      │  L = LHS->codegen()  ─▶  %a  (the argument value)
      │  R = RHS->codegen()  ─▶  %b
      ▼
   CreateFAdd(%a, %b)        ─▶  %addtmp = fadd double %a, %b
```

Note `<` produces an `i1` from `fcmp`, and we convert back to `double` (Toy has only one
type). This `CreateUIToFP` step is a small but instructive example of inserting the casts a
typed IR demands.

---

## 4. Function calls

```cpp
llvm::Value *CallExprAST::codegen() {
  // Look up the callee by name in the module's symbol table.
  llvm::Function *CalleeF = TheModule->getFunction(Callee);
  if (!CalleeF) { fprintf(stderr, "Unknown function: %s\n", Callee.c_str()); return nullptr; }

  if (CalleeF->arg_size() != Args.size()) {
    fprintf(stderr, "Incorrect # arguments passed\n"); return nullptr;
  }

  std::vector<llvm::Value*> ArgsV;
  for (auto &A : Args) {
    ArgsV.push_back(A->codegen());      // codegen each argument expression
    if (!ArgsV.back()) return nullptr;
  }
  return Builder->CreateCall(CalleeF, ArgsV, "calltmp");
}
```

```
   CallExprAST(fib, [n-1])
      │ getFunction("fib")  ─▶  the Function* (must be declared/defined)
      │ codegen each arg     ─▶  %subtmp = fsub double %n, 1.0
      ▼
   CreateCall(fib, [%subtmp]) ─▶  %calltmp = call double @fib(double %subtmp)
```

---

## 5. The `if`/then/else expression — emitting control flow

This is the most instructive node: it creates basic blocks, branches, and a **PHI** to merge
the two arms' values. It puts the entire SSA chapter into practice.

```cpp
llvm::Value *IfExprAST::codegen() {
  llvm::Value *CondV = Cond->codegen();
  if (!CondV) return nullptr;

  // Convert condition (a double) to an i1 by comparing != 0.0.
  CondV = Builder->CreateFCmpONE(
      CondV, llvm::ConstantFP::get(*TheContext, llvm::APFloat(0.0)), "ifcond");

  // Get the function we're currently emitting into.
  llvm::Function *TheFunction = Builder->GetInsertBlock()->getParent();

  // Create three blocks. 'then' is attached to the function now;
  // 'else' and 'merge' we create now but attach later (clarity).
  auto *ThenBB  = llvm::BasicBlock::Create(*TheContext, "then", TheFunction);
  auto *ElseBB  = llvm::BasicBlock::Create(*TheContext, "else");
  auto *MergeBB = llvm::BasicBlock::Create(*TheContext, "ifcont");

  Builder->CreateCondBr(CondV, ThenBB, ElseBB);     // br i1 %ifcond, then, else

  // --- THEN branch ---
  Builder->SetInsertPoint(ThenBB);
  llvm::Value *ThenV = Then->codegen();
  if (!ThenV) return nullptr;
  Builder->CreateBr(MergeBB);
  ThenBB = Builder->GetInsertBlock();   // codegen may have changed the current block

  // --- ELSE branch ---
  TheFunction->insert(TheFunction->end(), ElseBB);  // now attach else
  Builder->SetInsertPoint(ElseBB);
  llvm::Value *ElseV = Else->codegen();
  if (!ElseV) return nullptr;
  Builder->CreateBr(MergeBB);
  ElseBB = Builder->GetInsertBlock();

  // --- MERGE ---
  TheFunction->insert(TheFunction->end(), MergeBB);
  Builder->SetInsertPoint(MergeBB);
  llvm::PHINode *PN =
      Builder->CreatePHI(llvm::Type::getDoubleTy(*TheContext), 2, "iftmp");
  PN->addIncoming(ThenV, ThenBB);       // value if we came from 'then'
  PN->addIncoming(ElseV, ElseBB);       // value if we came from 'else'
  return PN;
}
```

The IR and CFG this produces, for `if x then 1.0 else 2.0`:

```
   entry:
     %ifcond = fcmp one double %x, 0.0
     br i1 %ifcond, label %then, label %else
                                  │
            ┌─────────────────────┴─────────────────────┐
            ▼                                           ▼
   then:                                         else:
     ; ThenV = 1.0                                 ; ElseV = 2.0
     br label %ifcont                              br label %ifcont
            │                                           │
            └─────────────────────┬─────────────────────┘
                                  ▼
   ifcont:
     %iftmp = phi double [ 1.0, %then ], [ 2.0, %else ]
     ; %iftmp is the value of the whole if-expression
```

Two subtleties worth their own callout:

```
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ (1) "ThenBB = Builder->GetInsertBlock()" AFTER codegen'ing the arm.      │
  │     Why? The arm might itself contain an if, leaving us in a DIFFERENT   │
  │     block than ThenBB. The PHI must reference the block control ACTUALLY │
  │     came from. Always re-read the current block before addIncoming.      │
  ├──────────────────────────────────────────────────────────────────────────┤
  │ (2) Blocks are created detached, then inserted into the function in      │
  │     program order for readable IR. Order doesn't affect correctness      │
  │     (the CFG is defined by branches, not textual order) but aids reading.│
  └──────────────────────────────────────────────────────────────────────────┘
```

---

## 6. Prototypes and functions

```cpp
llvm::Function *PrototypeAST::codegen() {
  // All Toy values are double. Build the function type: double(double, double, ...).
  std::vector<llvm::Type*> Doubles(Args.size(),
                                   llvm::Type::getDoubleTy(*TheContext));
  llvm::FunctionType *FT = llvm::FunctionType::get(
      llvm::Type::getDoubleTy(*TheContext), Doubles, /*isVarArg=*/false);

  llvm::Function *F = llvm::Function::Create(
      FT, llvm::Function::ExternalLinkage, Name, TheModule.get());

  // Give the parameters their source names (nice IR, and our symbol table uses them).
  unsigned Idx = 0;
  for (auto &Arg : F->args())
    Arg.setName(Args[Idx++]);
  return F;
}

llvm::Function *FunctionAST::codegen() {
  // Reuse an existing declaration (from 'extern') if present, else codegen the proto.
  llvm::Function *TheFunction = TheModule->getFunction(Proto->getName());
  if (!TheFunction) TheFunction = Proto->codegen();
  if (!TheFunction) return nullptr;

  // Create the entry block and point the builder at it.
  auto *BB = llvm::BasicBlock::Create(*TheContext, "entry", TheFunction);
  Builder->SetInsertPoint(BB);

  // Make params visible to the body via the symbol table.
  NamedValues.clear();
  for (auto &Arg : TheFunction->args())
    NamedValues[std::string(Arg.getName())] = &Arg;

  if (llvm::Value *RetVal = Body->codegen()) {
    Builder->CreateRet(RetVal);          // 'ret' terminates the entry block
    llvm::verifyFunction(*TheFunction);  // catch malformed IR early
    return TheFunction;
  }

  TheFunction->eraseFromParent();        // body failed; remove the half-built fn
  return nullptr;
}
```

End-to-end, `def add(a b) a + b;` becomes:

```llvm
define double @add(double %a, double %b) {
entry:
  %addtmp = fadd double %a, %b
  ret double %addtmp
}
```

And `def fib(n) if n < 2 then n else fib(n-1)+fib(n-2);` becomes (abbreviated):

```llvm
define double @fib(double %n) {
entry:
  %cmptmp = fcmp ult double %n, 2.0
  %booltmp = uitofp i1 %cmptmp to double
  %ifcond = fcmp one double %booltmp, 0.0
  br i1 %ifcond, label %then, label %else
then:
  br label %ifcont
else:
  %subtmp  = fsub double %n, 1.0
  %calltmp = call double @fib(double %subtmp)
  %subtmp1 = fsub double %n, 2.0
  %calltmp2= call double @fib(double %subtmp1)
  %addtmp  = fadd double %calltmp, %calltmp2
  br label %ifcont
ifcont:
  %iftmp = phi double [ %n, %then ], [ %addtmp, %else ]
  ret double %iftmp
}
```

That is a complete, valid LLVM function generated entirely by walking the AST. **This Module
is the launch pad for everything downstream.**

---

## 7. Adding mutable variables (the alloca pattern)

When you extend Toy with assignable locals (`var x = ...`), don't try to keep SSA by hand.
Use the alloca trick from the SSA chapter:

```cpp
// Helper: create an alloca in the function's ENTRY block (so mem2reg can promote it).
static llvm::AllocaInst *CreateEntryBlockAlloca(llvm::Function *F,
                                                const std::string &Name) {
  llvm::IRBuilder<> TmpB(&F->getEntryBlock(), F->getEntryBlock().begin());
  return TmpB.CreateAlloca(llvm::Type::getDoubleTy(*TheContext), nullptr, Name);
}
```

Then variable *reads* become `load`, *writes* become `store`, and `NamedValues` maps names
to the `AllocaInst*` instead of directly to a value:

```
   var x = 3;  x = x + 1;
   ───────────────────────────────────────────────
   %x = alloca double                 ; in entry block
   store double 3.0, ptr %x           ; var x = 3
   %x1 = load double, ptr %x          ; read x
   %add = fadd double %x1, 1.0
   store double %add, ptr %x          ; x = x + 1
```

Run `mem2reg` (see [../02-ir-and-passes/02-optimization-passes.md](../02-ir-and-passes/02-optimization-passes.md))
and all that memory traffic collapses into clean SSA with PHIs inserted automatically. You
get correct SSA *for free* without ever computing a dominance frontier.

---

## 8. The mapping table — your codegen cheat sheet

```
 ┌─────────────────────┬──────────────────────────────────┬──────────────────────────┐
 │ AST node            │ IRBuilder call                   │ IR emitted               │
 ├─────────────────────┼──────────────────────────────────┼──────────────────────────┤
 │ NumberExprAST       │ ConstantFP::get                  │ double 3.14              │
 │ VariableExprAST     │ (lookup) or CreateLoad           │ %x  or  load ...         │
 │ BinaryExprAST '+'   │ CreateFAdd                       │ fadd double ...          │
 │ BinaryExprAST '<'   │ CreateFCmpULT + CreateUIToFP     │ fcmp + uitofp            │
 │ CallExprAST         │ CreateCall                       │ call double @f(...)      │
 │ IfExprAST           │ CreateCondBr + CreatePHI         │ br + blocks + phi        │
 │ PrototypeAST        │ Function::Create                 │ define/declare double @f │
 │ FunctionAST         │ entry block + body + CreateRet   │ a full function          │
 │ mutable local       │ CreateAlloca + load/store        │ alloca/store/load        │
 └─────────────────────┴──────────────────────────────────┴──────────────────────────┘
```

This single table is the soul of a frontend. Everything else (more types, more operators,
loops, structs) is *more of the same*: a node maps to a short sequence of IRBuilder calls.

---

## Mental model checkpoint

1. What are the four global codegen objects and what does each do?
2. Why is expression codegen "post-order"?
3. In `IfExprAST::codegen`, why must you re-read `Builder->GetInsertBlock()` before adding
   PHI incoming values?
4. What three basic blocks does an `if` create, and what does the PHI in `ifcont` do?
5. Why does `FunctionAST::codegen` call `eraseFromParent()` on failure?
6. Describe the alloca pattern for mutable variables and which pass cleans it up.
7. From the cheat sheet: what IR does `a < b` emit and why two instructions?

Frontend complete. Next, the IRBuilder & passes → [../02-ir-and-passes/01-ir-builder.md](../02-ir-and-passes/01-ir-builder.md)

// hello_ir.cpp — build an LLVM Module in memory and print it.
// Demonstrates: LLVMContext, Module, IRBuilder, FunctionType, BasicBlock,
//               instruction creation, and the verifier (chapters 00.05, 02.01).
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

int main() {
  llvm::LLVMContext ctx;
  auto mod = std::make_unique<llvm::Module>("hello", ctx);
  llvm::IRBuilder<> b(ctx);

  // i32 square_plus_one(i32 x) { return x*x + 1; }
  auto *i32 = b.getInt32Ty();
  auto *fty = llvm::FunctionType::get(i32, {i32}, /*isVarArg=*/false);
  auto *fn = llvm::Function::Create(fty, llvm::Function::ExternalLinkage,
                                    "square_plus_one", mod.get());
  fn->getArg(0)->setName("x");

  auto *entry = llvm::BasicBlock::Create(ctx, "entry", fn);
  b.SetInsertPoint(entry);
  llvm::Value *x = fn->getArg(0);
  llvm::Value *mul = b.CreateMul(x, x, "mul");
  llvm::Value *add = b.CreateAdd(mul, b.getInt32(1), "add");
  b.CreateRet(add);

  // Verify the function is well-formed, then print the whole module as .ll text.
  if (llvm::verifyFunction(*fn, &llvm::errs())) {
    llvm::errs() << "function failed verification!\n";
    return 1;
  }
  mod->print(llvm::outs(), nullptr);
  return 0;
}

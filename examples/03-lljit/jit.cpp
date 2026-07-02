// jit.cpp — build a function and JIT-compile + call it in-process with ORC LLJIT.
// Demonstrates: LLJIT, ThreadSafeModule, addIRModule, lookup, calling a
//               JIT'd function pointer (chapters 04.02, 04.03).
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::orc;

// Build a module with: i32 times_plus(i32 x) { return x*3 + 7; }
static ThreadSafeModule makeModule(const DataLayout &DL) {
  auto ctx = std::make_unique<LLVMContext>();
  auto M = std::make_unique<Module>("jit_demo", *ctx);
  M->setDataLayout(DL); // MUST match the JIT's target

  IRBuilder<> b(*ctx);
  auto *i32 = b.getInt32Ty();
  auto *fn = Function::Create(FunctionType::get(i32, {i32}, false),
                              Function::ExternalLinkage, "times_plus", M.get());
  fn->getArg(0)->setName("x");
  auto *entry = BasicBlock::Create(*ctx, "entry", fn);
  b.SetInsertPoint(entry);
  Value *x = fn->getArg(0);
  Value *mul = b.CreateMul(x, b.getInt32(3), "mul");
  b.CreateRet(b.CreateAdd(mul, b.getInt32(7), "add"));

  return ThreadSafeModule(std::move(M), std::move(ctx));
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);

  // The JIT uses the backend → initialize host codegen.
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();

  // 1. Build the JIT (picks the host TargetMachine, assembles the layer stack).
  auto jitOrErr = LLJITBuilder().create();
  if (!jitOrErr) { logAllUnhandledErrors(jitOrErr.takeError(), errs(), "[jit] "); return 1; }
  std::unique_ptr<LLJIT> J = std::move(*jitOrErr);

  // 2. Add a module built with the JIT's data layout.
  if (auto err = J->addIRModule(makeModule(J->getDataLayout()))) {
    logAllUnhandledErrors(std::move(err), errs(), "[add] ");
    return 1;
  }

  // 3. Look up the symbol → triggers compilation → returns its address.
  auto sym = J->lookup("times_plus");
  if (!sym) { logAllUnhandledErrors(sym.takeError(), errs(), "[lookup] "); return 1; }

  // 4. Cast to a function pointer and CALL IT — native code, right now, in-process.
  auto *times_plus = sym->toPtr<int(int)>();
  for (int i = 0; i < 5; i++)
    outs() << "times_plus(" << i << ") = " << times_plus(i) << "\n";

  return 0;
}

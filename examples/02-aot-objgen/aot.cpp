// aot.cpp — full AOT in one file: build IR, optimize, emit a native .o.
// Demonstrates: TargetMachine, data layout, the new-PM optimization pipeline,
//               and addPassesToEmitFile (chapters 02.02, 03.02, 03.03).
//
// After running, link the produced object with the tiny C driver:
//   cc aot_out.o driver.c -o prog && ./prog
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"

using namespace llvm;

// Build:  i32 square_plus_one(i32 x) { return x*x + 1; }
static void buildIR(Module &M, LLVMContext &ctx) {
  IRBuilder<> b(ctx);
  auto *i32 = b.getInt32Ty();
  auto *fn = Function::Create(FunctionType::get(i32, {i32}, false),
                              Function::ExternalLinkage, "square_plus_one", &M);
  fn->getArg(0)->setName("x");
  auto *entry = BasicBlock::Create(ctx, "entry", fn);
  b.SetInsertPoint(entry);
  Value *x = fn->getArg(0);
  b.CreateRet(b.CreateAdd(b.CreateMul(x, x, "mul"), b.getInt32(1), "add"));
}

// Run the standard -O2 pipeline (new PassManager).
static void optimize(Module &M) {
  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;
  PassBuilder PB;
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
  ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(OptimizationLevel::O2);
  MPM.run(M, MAM);
}

int main() {
  LLVMContext ctx;
  Module M("aot_demo", ctx);

  // 1. Register the host backend.
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();

  // 2. Look up the target for this machine and build a TargetMachine.
  std::string triple = sys::getDefaultTargetTriple();
  M.setTargetTriple(triple);
  std::string err;
  const Target *target = TargetRegistry::lookupTarget(triple, err);
  if (!target) { errs() << "lookupTarget: " << err << "\n"; return 1; }

  TargetOptions opts;
  TargetMachine *TM =
      target->createTargetMachine(triple, "generic", "", opts, Reloc::PIC_);

  // 3. The module's data layout MUST match the TargetMachine.
  M.setDataLayout(TM->createDataLayout());

  // 4. Build and optimize the IR.
  buildIR(M, ctx);
  if (verifyModule(M, &errs())) { errs() << "bad IR\n"; return 1; }
  optimize(M);

  errs() << "--- optimized IR ---\n";
  M.print(errs(), nullptr);

  // 5. Emit a native object file via the (legacy) codegen pass manager.
  std::error_code EC;
  raw_fd_ostream dest("aot_out.o", EC, sys::fs::OF_None);
  if (EC) { errs() << "open aot_out.o: " << EC.message() << "\n"; return 1; }

  legacy::PassManager codegenPM;
  if (TM->addPassesToEmitFile(codegenPM, dest, nullptr,
                              CodeGenFileType::ObjectFile)) {
    errs() << "cannot emit object file\n";
    return 1;
  }
  codegenPM.run(M);
  dest.flush();

  errs() << "\nwrote aot_out.o\n"
         << "link & run with:\n  cc aot_out.o driver.c -o prog && ./prog\n";
  return 0;
}

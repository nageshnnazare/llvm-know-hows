// MulToAdd.cpp — a new-PassManager transform pass loadable into `opt`.
// Rewrites `mul %x, 2` into `add %x, %x`. Demonstrates: the new-PM pass
// interface, the collect→build→RAUW→erase rewrite idiom, and the pass-plugin
// registration callback (chapter 02.02).
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

struct MulToAddPass : PassInfoMixin<MulToAddPass> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &) {
    // Collect first — we'll erase instructions, so don't mutate while iterating.
    SmallVector<BinaryOperator *, 16> worklist;
    for (Instruction &I : instructions(F)) {
      auto *bo = dyn_cast<BinaryOperator>(&I);
      if (!bo || bo->getOpcode() != Instruction::Mul)
        continue;
      auto *c = dyn_cast<ConstantInt>(bo->getOperand(1));
      if (c && c->equalsInt(2))
        worklist.push_back(bo);
    }

    for (BinaryOperator *bo : worklist) {
      IRBuilder<> b(bo);                 // insert right before the old mul
      Value *x = bo->getOperand(0);
      Value *sum = b.CreateAdd(x, x, bo->getName() + ".m2a");
      bo->replaceAllUsesWith(sum);       // redirect every user to the new add
      bo->eraseFromParent();             // delete the old mul
    }

    if (worklist.empty())
      return PreservedAnalyses::all();
    errs() << "[mul-to-add] rewrote " << worklist.size()
           << " mul(s) in @" << F.getName() << "\n";
    return PreservedAnalyses::none();
  }

  // Run even on functions marked optnone, so the demo is easy to observe.
  static bool isRequired() { return true; }
};

} // namespace

// Plugin entry point: lets `opt -passes=mul-to-add` find this pass.
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "MulToAdd", "v1", [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef name, FunctionPassManager &FPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (name == "mul-to-add") {
                    FPM.addPass(MulToAddPass());
                    return true;
                  }
                  return false;
                });
          }};
}

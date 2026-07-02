# Example 04 · A custom optimization pass (`mul-to-add`)

A new-PassManager transform pass that rewrites `mul %x, 2` into `add %x, %x`, built as a
**loadable plugin** for `opt`. Shows the pass interface, the canonical
collect→build→RAUW→erase rewrite idiom, and plugin registration.

**Concepts:** `PassInfoMixin`, `run(Function&, FAM&)`, `PreservedAnalyses`,
`replaceAllUsesWith`/`eraseFromParent`, `llvmGetPassPluginInfo`,
`registerPipelineParsingCallback`. See chapter
[02.02](../../02-ir-and-passes/02-optimization-passes.md).

## Build & run

```bash
./build.sh
# manual:
cmake -S . -B build -DLLVM_DIR=$(llvm-config --cmakedir)
cmake --build build
opt -load-pass-plugin=./build/MulToAdd.so -passes=mul-to-add -S test.ll -o -
#                              ^ .dylib on macOS
```

## Expected output

```
[mul-to-add] rewrote 1 mul(s) in @double_it
; ... (module printed) ...
define i32 @double_it(i32 %x) {
entry:
  %t.m2a = add i32 %x, %x
  ret i32 %t.m2a
}

define i32 @triple_it(i32 %x) {
entry:
  %t = mul i32 %x, 3      ; unchanged — constant was 3, not 2
  ret i32 %t
}
```

## What to notice

- We **collect** matching instructions into a worklist *before* mutating, because we erase
  while we'd otherwise be iterating ([02.02 §6](../../02-ir-and-passes/02-optimization-passes.md)).
- `replaceAllUsesWith` then `eraseFromParent` is the universal IR-rewrite move — it rewires
  the use-def graph, then deletes the dead instruction.
- Returning `PreservedAnalyses::none()` when we changed IR tells the manager to invalidate
  cached analyses ([02.02 §7](../../02-ir-and-passes/02-optimization-passes.md)).
- The same pass class could be added directly in your compiler's pipeline (no plugin) via
  `FPM.addPass(MulToAddPass())`.
- Compose it with built-in passes: `-passes='mul-to-add,instcombine'`.

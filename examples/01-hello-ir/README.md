# Example 01 · Hello, IR

The "hello world" of LLVM: construct a `Module` in memory with `IRBuilder`, verify it, and
print it as textual `.ll`. No optimization, no codegen — just the core IR-building API.

**Concepts:** `LLVMContext`, `Module`, `IRBuilder`, `FunctionType`, `Function`, `BasicBlock`,
`CreateMul`/`CreateAdd`/`CreateRet`, `verifyFunction`. See chapters
[00.05](../../00-foundations/05-environment-setup.md) and
[02.01](../../02-ir-and-passes/01-ir-builder.md).

## Build & run

```bash
./build.sh
# or manually:
cmake -S . -B build -DLLVM_DIR=$(llvm-config --cmakedir)
cmake --build build
./build/hello_ir
```

## Expected output

```llvm
; ModuleID = 'hello'
source_filename = "hello"

define i32 @square_plus_one(i32 %x) {
entry:
  %mul = mul i32 %x, %x
  %add = add i32 %mul, 1
  ret i32 %add
}
```

## What to notice

- Every value is typed (`i32`).
- `CreateMul`/`CreateAdd` return `Value*` you immediately reuse — the use-def graph forming.
- The function ends in a terminator (`ret`) — the basic-block rule from
  [00.03](../../00-foundations/03-llvm-ir-essentials.md).
- This exact `Module` is what examples 02 (AOT) and 03 (JIT) take to the next stage.

# Example 02 · AOT — IR to a native object file

The complete AOT story in one program: build IR, run the `-O2` pipeline, create a
`TargetMachine`, and emit a real `.o` via `addPassesToEmitFile`. Then a tiny C `driver.c`
links against it and runs — proving the compiler is gone by runtime.

**Concepts:** `TargetMachine`, target triple, `createDataLayout`, the new-PM `-O2` pipeline,
the legacy codegen PassManager, `CodeGenFileType::ObjectFile`, linking & symbols. See
chapters [02.02](../../02-ir-and-passes/02-optimization-passes.md),
[03.02](../../03-aot/02-target-machine-codegen.md),
[03.03](../../03-aot/03-building-an-aot-compiler.md),
[03.04](../../03-aot/04-linking-and-runtime.md).

## Build & run

```bash
./build.sh
# manual:
cmake -S . -B build -DLLVM_DIR=$(llvm-config --cmakedir)
cmake --build build
./build/aot                       # emits aot_out.o
cc aot_out.o driver.c -o prog     # link with the C driver
./prog
```

## Expected output

```
square_plus_one(0) = 1
square_plus_one(1) = 2
square_plus_one(2) = 5
square_plus_one(3) = 10
square_plus_one(4) = 17
```

## What to notice

- Two pass managers coexist: **new PM** for IR optimization, **legacy PM** for codegen — see
  [03.02 §5](../../03-aot/02-target-machine-codegen.md).
- `aot_out.o` has `square_plus_one` as a **defined** symbol; `driver.c` references it as
  **undefined**; the linker matches them ([03.04](../../03-aot/04-linking-and-runtime.md)).
- Inspect the artifact:
  ```bash
  llvm-objdump -d aot_out.o        # see the machine code
  llvm-nm aot_out.o                 # T square_plus_one (defined in text)
  ```
- `./prog` runs with **no compiler present** — the definition of AOT.

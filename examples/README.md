# Examples — Buildable, Runnable Code

Each example is self-contained: a `README.md`, a `CMakeLists.txt`, a `build.sh`, and the
source. They are referenced from the chapters. Build any of them with the shared recipe
below.

## Prerequisites

- LLVM 17/18+ installed (see [../00-foundations/05-environment-setup.md](../00-foundations/05-environment-setup.md)).
- `llvm-config` on your PATH (verify: `llvm-config --version`).
- CMake 3.20+ and a C++17 compiler.

## Universal build recipe

```bash
cd <example-dir>
cmake -S . -B build -DLLVM_DIR=$(llvm-config --cmakedir)
cmake --build build
./build/<binary>
```

Or just run the provided script:

```bash
cd <example-dir> && ./build.sh
```

## The examples

| Dir | Demonstrates | Chapter |
|-----|--------------|---------|
| [01-hello-ir](01-hello-ir/) | Build a `Module` with `IRBuilder` and print it. The "hello world" of LLVM. | 00.05, 02.01 |
| [02-aot-objgen](02-aot-objgen/) | Generate IR, optimize it, emit a native `.o`, link, and run. Full AOT in one file. | 03.02, 03.03 |
| [03-lljit](03-lljit/) | Build a function and JIT-compile + call it in-process with ORC `LLJIT`. | 04.02, 04.03 |
| [04-custom-pass](04-custom-pass/) | A new-PassManager transform pass (`mul-to-add`) loadable into `opt`. | 02.02 |

Each example's `README.md` explains exactly what it does, the expected output, and which
chapter concepts it makes concrete.

> These intentionally generate IR programmatically (rather than parsing the Toy language) so
> each is a single short file you can read in one sitting. The Toy frontend lives in section
> 01; wiring it to these backends is the capstone exercise (section 07).

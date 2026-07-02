# Example 03 · JIT — compile and call in-process with ORC LLJIT

Build a function, hand it to ORC's `LLJIT`, look up its symbol (which triggers compilation),
and call the returned function pointer — all in the same process. Compare with example 02:
identical frontend/IR, but the bytes go to **memory and a call** instead of a **file and a
link**.

**Concepts:** `LLJIT`, `LLJITBuilder`, `ThreadSafeModule`, matching `DataLayout`,
`addIRModule`, `lookup`, `toPtr`, calling JIT'd code. See chapters
[04.01](../../04-jit/01-jit-theory.md), [04.02](../../04-jit/02-orc-architecture.md),
[04.03](../../04-jit/03-building-a-jit.md).

## Build & run

```bash
./build.sh
# manual:
cmake -S . -B build -DLLVM_DIR=$(llvm-config --cmakedir)
cmake --build build
./build/jit
```

## Expected output

```
times_plus(0) = 7
times_plus(1) = 10
times_plus(2) = 13
times_plus(3) = 16
times_plus(4) = 19
```

## What to notice

- `J->lookup("times_plus")` is where compilation actually happens — *on request*
  ([04.02 §2](../../04-jit/02-orc-architecture.md)).
- The Module's `DataLayout` is set from `J->getDataLayout()` — a mandatory match
  ([04.03 §1](../../04-jit/03-building-a-jit.md)).
- `times_plus(i)` jumps into machine code generated *milliseconds ago*, in this process.
- This is the same `square_plus_one`-style IR as example 02 — the **frontend and optimizer
  are shared; only delivery differs** ([the unifying idea](../../README.md)).
- To let JIT'd code call `printf`/`sin`, add a `DynamicLibrarySearchGenerator` to the main
  JITDylib ([04.03 §2](../../04-jit/03-building-a-jit.md)).

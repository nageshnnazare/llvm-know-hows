# 03.03 · Building a Complete AOT Compiler

> We assemble everything — frontend, optimizer, object emission — into one working AOT
> compiler for the Toy language. Input: a `.toy` file. Output: a native executable you can
> run. This is the full "compile now, run later" loop end to end.

---

## 1. The architecture

```
   ┌──────────────────────────── toyc (our AOT compiler) ─────────────────────────────┐
   │                                                                                  │
   │  .toy file ─▶ Lexer ─▶ Parser ─▶ AST ─▶ Codegen ─▶ Module                        │
   │              (01.01)  (01.02)         (01.03)                                    │
   │                                                       │                          │
   │                                                       ▼                          │
   │                                            optimizeModule (-O2)  (02.02)         │
   │                                                       │                          │
   │                                                       ▼                          │
   │                                            moduleToObjectFile     (03.02)        │
   │                                                       │                          │
   │                                                       ▼                          │
   │                                                  output.o                        │
   └───────────────────────────────────────────────────────┬──────────────────────────┘
                                                           │ invoke linker (clang/cc)
                                                           ▼
                                                      executable  ──▶ ./output
```

We reuse the lexer/parser/codegen from section 01 verbatim. The *new* code is the driver
that orchestrates: parse the file, codegen all functions into one Module, optimize, emit a
`.o`, and link.

---

## 2. The driver `main`

```cpp
// toyc.cpp — the AOT driver
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <string>

// (declared in our frontend translation units from section 01)
extern std::unique_ptr<llvm::Module> TheModule;
extern void InitializeModule();
void runFrontend(FILE *input);                 // lexes+parses+codegens whole file
bool optimizeModule(llvm::Module &, int optLevel);   // from 02.02 (wrapper)
bool moduleToObjectFile(llvm::Module &, const std::string &out);  // from 03.02

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: toyc file.toy [-o out]\n"); return 1; }
  const char *srcPath = argv[1];
  std::string objPath = "output.o";

  FILE *in = fopen(srcPath, "r");
  if (!in) { perror("open source"); return 1; }

  InitializeModule();          // create LLVMContext, Module, IRBuilder
  runFrontend(in);             // fills TheModule with functions
  fclose(in);

  if (llvm::verifyModule(*TheModule, &llvm::errs())) {   // catch malformed IR
    fprintf(stderr, "internal error: invalid IR generated\n");
    return 1;
  }

  optimizeModule(*TheModule, /*optLevel=*/2);            // -O2 pipeline

  if (!moduleToObjectFile(*TheModule, objPath))          // emit .o
    return 1;

  fprintf(stderr, "wrote %s\n", objPath.c_str());
  return 0;
}
```

`runFrontend` is the section-01 `MainLoop`, with each `Handle*` calling `codegen()`:

```cpp
void runFrontend(FILE *in) {
  setInput(in);            // make gettok read from this file
  getNextToken();          // prime CurTok
  while (true) {
    switch (CurTok) {
    case tok_eof: return;
    case ';': getNextToken(); break;
    case tok_def:
      if (auto F = ParseDefinition()) F->codegen();
      else getNextToken();                          // skip on error
      break;
    case tok_extern:
      if (auto P = ParseExtern()) P->codegen();
      else getNextToken();
      break;
    default:
      if (auto F = ParseTopLevelExpr()) F->codegen();
      else getNextToken();
      break;
    }
  }
}
```

> Difference from a JIT/REPL: here we *accumulate everything into one Module*, then optimize
> and emit once. A JIT would compile each top-level entry immediately and run it. Same
> frontend, different "what to do with the Module."

---

## 3. The `main` problem: Toy needs an entry point

Toy programs are functions and top-level expressions; there's no `main`. For an executable we
must supply one. Two clean options:

```
   OPTION A (synthesize main in IR): generate an i32 @main() that calls the
   anonymous top-level expr(s) and prints results / returns 0.

   OPTION B (provide a C driver): emit only Toy functions; write a small
   main.c that calls them, and link both .o's.
```

Option A, generating `main` ourselves, keeps it self-contained. Suppose we want
`main` to call a Toy function `@main_expr` (a renamed `__anon_expr`) and print it:

```cpp
// After frontend, synthesize: i32 main() { printf("%f\n", main_expr()); return 0; }
void synthesizeMain(llvm::Module &M, llvm::LLVMContext &Ctx) {
  llvm::IRBuilder<> B(Ctx);
  auto *i32 = B.getInt32Ty();

  // declare i32 @printf(ptr, ...)
  auto *printfTy = llvm::FunctionType::get(i32, {B.getPtrTy()}, /*vararg=*/true);
  auto printf = M.getOrInsertFunction("printf", printfTy);

  auto *mainTy = llvm::FunctionType::get(i32, false);
  auto *mainFn = llvm::Function::Create(mainTy, llvm::Function::ExternalLinkage,
                                        "main", &M);
  auto *bb = llvm::BasicBlock::Create(Ctx, "entry", mainFn);
  B.SetInsertPoint(bb);

  llvm::Function *expr = M.getFunction("main_expr");   // a double() Toy fn
  llvm::Value *result = B.CreateCall(expr, {}, "res");
  llvm::Value *fmt = B.CreateGlobalStringPtr("%f\n");
  B.CreateCall(printf, {fmt, result});
  B.CreateRet(B.getInt32(0));
}
```

```
   define i32 @main() {
   entry:
     %res = call double @main_expr()
     %0 = call i32 (ptr, ...) @printf(ptr @fmt, double %res)
     ret i32 0
   }
```

Now the Module has a real entry point referencing `printf` (resolved from libc at link).

---

## 4. Linking: from `.o` to executable

LLVM's codegen stops at the object file. To make a runnable program you **link** it with the
C runtime and libraries. Two approaches:

### (a) Shell out to the system compiler (simplest, robust)

`clang`/`cc` know how to find crt0, libc, the dynamic linker, and the right flags per
platform. Let them drive the link:

```cpp
#include <cstdlib>
bool linkExecutable(const std::string &obj, const std::string &exe) {
  std::string cmd = "cc " + obj + " -o " + exe + " -lm";   // -lm for Toy's extern sin etc.
  fprintf(stderr, "linking: %s\n", cmd.c_str());
  return std::system(cmd.c_str()) == 0;
}
```

```bash
# what that command does, conceptually:
>>> cc output.o -o output -lm
        │
        ├─ adds crt1.o / crti.o (startup: _start → main)
        ├─ resolves printf, sin from libc/libm
        ├─ lays out segments, sets entry point
        └─ produces a dynamically-linked ELF/Mach-O executable
```

### (b) Use `lld` programmatically or as a tool

LLVM's own linker `lld` can be invoked as `ld.lld` (ELF), `ld64.lld` (Mach-O), `lld-link`
(COFF), or embedded as a library. For most projects, shelling to `clang`/`cc` (which may
*use* lld via `-fuse-ld=lld`) is the pragmatic choice. Deep dive on what linking actually
does in [04-linking-and-runtime.md](04-linking-and-runtime.md).

---

## 5. End-to-end run

```bash
# build the compiler (CMake template from 00.05, linking core/support/native/passes)
>>> cmake -S . -B build -DLLVM_DIR=$(llvm-config --cmakedir) && cmake --build build

# write a Toy program
>>> cat > prog.toy <<'EOF'
def square(x) x * x;
def main_expr() square(7) + 1;
EOF

# compile it (frontend → IR → -O2 → object), then it links
>>> ./build/toyc prog.toy -o prog
wrote output.o
linking: cc output.o -o prog -lm

# run the NATIVE binary — no compiler involved anymore
>>> ./prog
50.000000
```

```
   prog.toy ──[toyc: frontend+opt+codegen]──▶ output.o ──[cc: link]──▶ prog
                                                                         │
                                                                  ./prog (AOT!)
                                                                  prints 50.0
```

The compiler is *done* before `./prog` runs. That's AOT.

---

## 6. Inspecting what we built

```bash
>>> llvm-objdump -d output.o | head -20      # see square's machine code
>>> llvm-nm prog | grep -E 'square|main'     # symbols in the linked binary
>>> ./prog ; echo "exit=$?"                  # run & check exit status
```

Bump optimization and watch IR/codegen change — a great learning loop:

```bash
# Emit IR instead of object to read it:
# (add a --emit-llvm flag to toyc that calls TheModule->print(outs()) and exits)
>>> ./build/toyc prog.toy --emit-llvm
define double @square(double %x) {
entry:
  %multmp = fmul double %x, %x
  ret double %multmp
}
...
```

---

## 7. What a *production* AOT driver adds

Our toyc is the skeleton. Real AOT compilers (clang, rustc) layer on:

```
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ • A DRIVER that orchestrates multiple files, dependency order, caching.  │
  │ • Debug info (DWARF) so debuggers map machine code ↔ source lines.       │
  │ • Multiple translation units + a real linker invocation with flags.      │
  │ • LTO: emit bitcode, optimize whole-program at link (02.03 §7).          │
  │ • PGO: instrument, collect profile, recompile with it.                   │
  │ • Diagnostics with source locations, error recovery, warnings.           │
  │ • Target/CPU/feature flags (-march=native, -mcpu=...).                   │
  │ • Sanitizers, coverage, stack protectors (extra instrumentation passes). │
  └──────────────────────────────────────────────────────────────────────────┘
```

But the *core* is exactly what you built: front-end → Module → optimize → object → link.
Everything else is engineering around that spine.

---

## Mental model checkpoint

1. In the AOT driver, why do we accumulate all functions into one Module before optimizing,
   unlike a JIT?
2. Why does a Toy program need a synthesized (or external) `main`, and what does it
   reference?
3. Why does the AOT compiler shell out to `cc`/`clang` for linking instead of stopping at the
   `.o`?
4. Trace the artifacts from `prog.toy` to a running process.
5. After `./prog` starts, is the compiler involved at all? Why does that define AOT?
6. Name four features a production AOT driver adds beyond this skeleton.

Next → [04-linking-and-runtime.md](04-linking-and-runtime.md)

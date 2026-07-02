# 00.02 · The Compilation Pipeline, End to End

> We trace a single line of source code all the way to running machine instructions,
> naming every stage, every data structure, and every LLVM tool that operates there.
> This is the map you'll refer back to from every other chapter.

---

## 1. The full pipeline at a glance

```
 SOURCE        TOKENS         AST            LLVM IR        OPT IR        MACHINE
 TEXT    ───▶  (lexing) ───▶  (parsing) ──▶  (codegen) ──▶ (passes) ──▶  CODE
 "a+b*2"       [a][+][b]      BinOp(+,        %t=mul...     %t=mul...     imul/add
               [*][2]          a,BinOp(*,     %r=add...     (cleaned)     bytes
                               b,2))
   │              │              │              │             │            │
   ▼              ▼              ▼              ▼             ▼            ▼
 .c/.rs/...   Token stream   Tree of nodes   Module/Func   Module/Func  .o / mem
              ───────────    ────────────    ──────────    ──────────   ────────
              FRONTEND        FRONTEND        FRONTEND      MIDDLE-END   BACKEND
              (you)           (you)           (you+LLVM)    (LLVM)       (LLVM)
```

Each arrow is a *transformation*; each box is a *representation*. A compiler is just a
sequence of representations, each lower-level and closer to the machine than the last.

---

## 2. Stage-by-stage, with the canonical example

We compile this function the whole way down:

```c
int square_plus_one(int x) {
    return x * x + 1;
}
```

### Stage 0 — Source text

Just bytes in a file. Meaningless to the machine. To us it's a stream of characters:

```
 i n t   s q u a r e _ p l u s _ o n e ( i n t   x )   {   r e t u r n   x   *   x ...
 └───────────────────────────── raw characters ────────────────────────────────────┘
```

### Stage 1 — Lexing (a.k.a. tokenizing / scanning)

A **lexer** groups characters into **tokens** — the atoms of the language. Whitespace and
comments are discarded.

```
   characters                          tokens
  ─────────────                       ────────────────────────────────
  "int"             ───────────────▶   KW_INT
  "square_plus_one"                    IDENT("square_plus_one")
  "("                                  LPAREN
  "int"                                KW_INT
  "x"                                  IDENT("x")
  ")"                                  RPAREN
  "{"                                  LBRACE
  "return"                             KW_RETURN
  "x"                                  IDENT("x")
  "*"                                  STAR
  "x"                                  IDENT("x")
  "+"                                  PLUS
  "1"                                  INT_LIT(1)
  ";"                                  SEMI
  "}"                                  RBRACE
```

Tool/where: **you write this** (or use a generator like Flex). Covered in
[../01-frontend/01-lexer.md](../01-frontend/01-lexer.md).

### Stage 2 — Parsing → AST

A **parser** consumes tokens and builds an **Abstract Syntax Tree** reflecting the grammar
and operator precedence. `*` binds tighter than `+`, so the tree nests accordingly:

```
                    FunctionDecl "square_plus_one"
                          │ returns int, param: x:int
                          ▼
                       ReturnStmt
                          │
                          ▼
                     BinaryExpr (+)
                     ╱           ╲
                    ▼             ▼
              BinaryExpr (*)    IntLiteral(1)
              ╱          ╲
             ▼            ▼
        VarRef(x)     VarRef(x)
```

The tree *is* the meaning: "return (the sum of (x times x) and 1)." Tool/where: **you write
this**, see [../01-frontend/02-parser-ast.md](../01-frontend/02-parser-ast.md).

### Stage 3 — Codegen: AST → LLVM IR

You walk the AST and, for each node, call `IRBuilder` methods that emit IR. The tree above
becomes:

```llvm
define i32 @square_plus_one(i32 %x) {
entry:
  %mul = mul i32 %x, %x      ; from BinaryExpr(*)
  %add = add i32 %mul, 1     ; from BinaryExpr(+) with IntLiteral(1)
  ret i32 %add               ; from ReturnStmt
}
```

The mapping is delightfully direct:

```
   AST node              IRBuilder call                 IR produced
  ────────────         ───────────────────            ──────────────
  BinaryExpr(*)   ──▶  Builder.CreateMul(L,R)    ──▶  %mul = mul i32 ...
  IntLiteral(1)   ──▶  ConstantInt::get(i32,1)   ──▶  i32 1
  BinaryExpr(+)   ──▶  Builder.CreateAdd(L,R)    ──▶  %add = add i32 ...
  ReturnStmt      ──▶  Builder.CreateRet(V)      ──▶  ret i32 %add
```

Tool/where: **you + LLVM's IRBuilder**, see
[../01-frontend/03-ast-to-ir.md](../01-frontend/03-ast-to-ir.md). On the command line, the
equivalent of "see the IR clang produces" is:

```bash
>>> clang -S -emit-llvm square.c -o square.ll
```

### Stage 4 — Optimization (the middle end)

LLVM runs **passes** over the IR. For `x*x+1` there's little to do, but consider a constant
case `square_plus_one(3)` inlined — passes would fold it to `10`. General example of what
passes do:

```
  before mem2reg/instcombine          after
  ──────────────────────────         ────────────────────
  %a = alloca i32                     ; promoted to SSA register,
  store i32 %x, %a                    ; loads/stores eliminated,
  %t = load i32, %a                   %mul = mul i32 %x, %x
  %mul = mul i32 %t, %t               ...
```

The optimizer is *target-independent*: the same passes help x86, ARM, RISC-V alike.
Tool/where: **LLVM**, driven by you via the PassManager, or on the CLI:

```bash
>>> opt -O2 -S square.ll -o square.opt.ll
```

See [../02-ir-and-passes/02-optimization-passes.md](../02-ir-and-passes/02-optimization-passes.md).

### Stage 5 — Backend: IR → machine code

The backend lowers target-independent IR to a specific CPU's instructions. The pipeline
*inside* the backend:

```
  LLVM IR
     │  instruction selection (match IR patterns to machine ops)
     ▼
  SelectionDAG / GlobalISel
     │  scheduling, then...
     ▼
  MachineInstr (MIR)   ← still virtual registers
     │  register allocation (map virtual regs → physical regs, spill if needed)
     ▼
  MachineInstr (physical regs)
     │  MC layer: encode to bytes / emit asm
     ▼
  object file (.o)  OR  bytes in memory
```

For x86-64, our function becomes something like:

```asm
square_plus_one:
    imull   %edi, %edi      # x = x * x   (edi holds first arg)
    leal    1(%rdi), %eax   # eax = x + 1
    retq
```

Tool/where: **LLVM**, via `TargetMachine`. On the CLI:

```bash
>>> llc square.opt.ll -o square.s        # to assembly
>>> llc -filetype=obj square.opt.ll -o square.o   # straight to object
```

See section [06](../06-backend/01-codegen-pipeline.md) for the deep dive.

### Stage 6 — The fork: AOT vs JIT

This is the *only* place AOT and JIT diverge:

```
                          machine code is ready
                                   │
                ┌──────────────────┴──────────────────┐
                ▼                                     ▼
   AOT: bytes → object file on disk         JIT: bytes → mmap'd RWX memory
        │                                        │
        │ linker (ld/lld) combines .o's          │ apply relocations in-process
        │ + libraries → executable               │ (fixups), flush icache
        ▼                                        ▼
   ./a.out   (run later, separate process)   void(*fp)() = addr; fp();  // run NOW
```

---

## 3. The same pipeline, three ways — side by side

```
 ┌───────────────┬────────────────────────┬────────────────────────┬──────────────────────┐
 │ Stage         │ AOT compiler           │ JIT compiler           │ MLIR-based           │
 ├───────────────┼────────────────────────┼────────────────────────┼──────────────────────┤
 │ frontend      │ your lexer/parser      │ same                   │ your lexer/parser    │
 │ produce IR    │ AST → LLVM IR          │ AST → LLVM IR          │ AST → MLIR dialect   │
 │ optimize      │ PassManager on Module  │ PassManager on Module  │ MLIR passes, then    │
 │               │                        │ (often per-function)   │ lower to LLVM dialect│
 │ lower to LLVM │  —                     │  —                     │ MLIR → LLVM IR       │
 │ codegen       │ TargetMachine→.o       │ ORC compiles to mem    │ then AOT or JIT      │
 │ deliver       │ link → executable      │ fn ptr, call now       │ (reuses AOT/JIT)     │
 │ when runs     │ later, maybe elsewhere │ immediately, in-proc   │ either               │
 └───────────────┴────────────────────────┴────────────────────────┴──────────────────────┘
```

Notice columns 1 and 2 (AOT/JIT) are identical until the last two rows. MLIR just prepends
extra high-level stages before joining the LLVM IR highway.

---

## 4. The CLI toolchain mirrors the pipeline

You can do *by hand* with command-line tools exactly what your code does via the API. This
is the best way to *see* each stage. Memorize this table:

```
 ┌─────────────────┬──────────────────────────────────────────────────────────┐
 │ Tool            │ What it does (which arrow in the pipeline)               │
 ├─────────────────┼──────────────────────────────────────────────────────────┤
 │ clang -emit-llvm│ source → LLVM IR (full C/C++ frontend)                   │
 │ opt             │ LLVM IR → LLVM IR (run optimization/analysis passes)     │
 │ llc             │ LLVM IR → assembly or object (the backend)               │
 │ llvm-as         │ textual .ll → bitcode .bc                                │
 │ llvm-dis        │ bitcode .bc → textual .ll                                │
 │ lli             │ run LLVM IR directly via the JIT (interpreter/JIT)       │
 │ llvm-mc         │ assembly ↔ machine code bytes (the MC layer)             │
 │ lld / ld        │ object files → executable (linking)                      │
 │ mlir-opt        │ MLIR → MLIR (run dialect passes / conversions)           │
 │ mlir-translate  │ MLIR (LLVM dialect) → LLVM IR                            │
 └─────────────────┴──────────────────────────────────────────────────────────┘
```

A complete hand-run of the pipeline:

```bash
>>> clang -S -emit-llvm square.c -o square.ll      # frontend
>>> opt -O2 -S square.ll -o square.opt.ll          # middle end
>>> llc -filetype=obj square.opt.ll -o square.o     # backend
>>> clang square.o -o square                        # link (clang drives the linker)
>>> ./square                                        # run (AOT!)
```

And the JIT shortcut for comparison:

```bash
>>> lli square.opt.ll      # JIT-compiles and runs in one shot, in-process
```

---

## 5. Where the abstraction level drops

A useful way to feel the pipeline: watch the abstraction level fall monotonically.

```
 abstraction
   level
    high │ source       "x * x + 1"  — human concepts, types, scopes
         │ AST          tree of meaning — still structured, language-specific
         │ MLIR (opt.)  high dialects (tensors, loops) — domain concepts  ← MLIR lives here
         │ LLVM IR      typed SSA, RISC-like — language & target neutral
         │ MachineInstr target ops, virtual regs — target-specific, pre-regalloc
         │ MC / asm     real registers, real opcodes
    low  │ bytes        relocatable/executable machine code
         └────────────────────────────────────────────────────────────────▶ pipeline
```

MLIR's whole pitch is: *don't drop straight from AST to LLVM IR; insert intermediate levels
where domain optimizations are natural.* More in [section 05](../05-mlir/01-mlir-theory.md).

---

## Mental model checkpoint

1. List the six representations from source to bytes, in order.
2. Which stages do *you* implement vs. which does LLVM provide?
3. What is the single divergence point between AOT and JIT?
4. Match each tool to a pipeline arrow: `opt`, `llc`, `clang -emit-llvm`, `lli`, `lld`.
5. Where in the abstraction-level diagram does MLIR add value, and why there?
6. Reproduce, from memory, the 5-command CLI run that takes a `.c` file to a running binary.

Next → [03-llvm-ir-essentials.md](03-llvm-ir-essentials.md)

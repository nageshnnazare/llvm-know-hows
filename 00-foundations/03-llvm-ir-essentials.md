# 00.03 · LLVM IR Essentials — Read and Write It Fluently

> You cannot build an AOT, JIT, or MLIR compiler without being fluent in LLVM IR the way a
> C programmer is fluent in C. This chapter makes you fluent: every structural concept,
> every common instruction, the type system, and the gotchas — all with diagrams.

---

## 1. The anatomy of a Module

A `Module` is one translation unit. Here is a complete, annotated module:

```llvm
; ─── Module-level metadata ────────────────────────────────────────────
target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx14.0.0"

; ─── A global variable ────────────────────────────────────────────────
@counter = global i32 0                  ; a mutable global int, init 0
@.str    = private constant [6 x i8] c"hi!\0A\00"   ; a string literal

; ─── An external function declaration (defined elsewhere, e.g. libc) ───
declare i32 @puts(ptr)

; ─── A function definition ────────────────────────────────────────────
define i32 @main() {
entry:
  %r = call i32 @puts(ptr @.str)
  ret i32 0
}
```

Structurally:

```
   Module
     ├── target datalayout    (how types are sized/aligned — see §6)
     ├── target triple        (arch-vendor-os — who we compile for)
     ├── GlobalVariables      @counter, @.str
     ├── Function declarations @puts          (no body, just a signature)
     └── Function definitions  @main          (has a body of basic blocks)
```

> `@` = global/module-scope symbol (functions, globals). `%` = local (within a function).
> `;` starts a comment to end of line.

---

## 2. Functions, basic blocks, and the terminator rule

A function body is a list of **basic blocks**. A basic block is a **maximal straight-line
sequence** of instructions: control enters only at the top (via its label) and leaves only
at the bottom (via exactly one **terminator** instruction).

```
  define i32 @f(i32 %n) {
  entry:                         ┌─ BasicBlock "entry"
    %cmp = icmp slt i32 %n, 0    │  non-terminator instructions...
    br i1 %cmp, label %neg, ...  └─ TERMINATOR (br) — exactly one, at the end
  neg:                           ┌─ BasicBlock "neg"
    %z = sub i32 0, %n           │
    br label %done               └─ TERMINATOR
  ...
  }
```

The iron law:

```
  ┌────────────────────────────────────────────────────────────────────┐
  │ EVERY basic block ends with EXACTLY ONE terminator instruction.    │
  │ Terminators: ret, br, switch, indirectbr, invoke, unreachable, ... │
  │ No instruction may appear AFTER a terminator in the same block.    │
  │ No terminator may appear in the MIDDLE of a block.                 │
  └────────────────────────────────────────────────────────────────────┘
```

This is what makes a block a *node* in the control-flow graph: it's an indivisible unit of
straight-line execution. (Full CFG/SSA treatment in
[04-ssa-and-cfg.md](04-ssa-and-cfg.md).)

---

## 3. The type system

LLVM IR is strongly typed. Every value has a type known at compile time.

```
  PRIMITIVE                         AGGREGATE & DERIVED
  ─────────                         ───────────────────
  i1    1-bit  (booleans)           [4 x i32]     array of 4 ints
  i8    8-bit                       {i32, i8, ptr} struct (anonymous)
  i16   16-bit                      <4 x float>   VECTOR (SIMD!) of 4 floats
  i32   32-bit                      ptr           opaque pointer (LLVM 15+)
  i64   64-bit                      i32 (i32, i8) function type
  iN    arbitrary N-bit             label         a basic block reference
  half/bfloat/float/double/fp128    void          no value
  x86_fp80, ppc_fp128
```

Key modern points:

- **Integers are sign-agnostic.** `i32` is just 32 bits. Signedness lives in the
  *instruction* (`sdiv` vs `udiv`, `slt` vs `ult`), not the type. This is a frequent
  surprise for newcomers.

- **Opaque pointers (LLVM 15+):** there is just `ptr`, not `i32*` or `%struct.Foo*`. The
  pointee type is no longer part of the pointer type. You specify the type at the *operation*
  (e.g. `load i32, ptr %p` says "load an i32 from %p"). Old tutorials show typed pointers
  like `i32*`; those are gone.

```
   OLD (pre-15, do not use)        NEW (LLVM 15+)
   ───────────────────────        ─────────────────────────
   %p = alloca i32                 %p = alloca i32
   store i32 5, i32* %p            store i32 5, ptr %p
   %v = load i32, i32* %p          %v = load i32, ptr %p
   ^^^ pointer carried "i32"        ^^^ pointer is just `ptr`;
                                        the load states the type
```

- **Vectors `<N x T>`** are first-class for SIMD; ops like `add <4 x i32>` operate
  lane-wise. This is how autovectorization expresses itself.

---

## 4. The instruction zoo (the ones you'll emit constantly)

Grouped by purpose. These cover ~90% of what a frontend emits.

### Arithmetic & logical

```llvm
%s = add  i32 %a, %b        ; integer add        (also sub, mul)
%d = sdiv i32 %a, %b        ; SIGNED divide      (udiv = unsigned)
%r = srem i32 %a, %b        ; signed remainder
%f = fadd double %x, %y     ; float add          (fsub, fmul, fdiv)
%a2= and  i32 %a, %b        ; bitwise            (or, xor)
%sh= shl  i32 %a, 2         ; shift left         (lshr logical, ashr arithmetic)
```

Optional **flags** sharpen semantics for the optimizer:

```llvm
%s = add nsw i32 %a, %b     ; "no signed wrap": overflow is UB → optimizer may assume none
%s = add nuw i32 %a, %b     ; "no unsigned wrap"
%f = fmul fast double %x,%y ; "fast math": allow reassociation, ignore NaN/inf, etc.
```

### Memory

```llvm
%p = alloca i32              ; allocate one i32 on the STACK; result is a ptr
store i32 42, ptr %p         ; write 42 to *p
%v = load i32, ptr %p        ; read *p into %v   (note: type stated here)
%e = getelementptr i32, ptr %base, i64 3   ; address arithmetic: &base[3]
```

`getelementptr` (GEP) is the trickiest. It computes an **address**, performs **no memory
access**, and is type-aware about element sizes:

```
   getelementptr i32, ptr %base, i64 3
   ─────────────  ───  ──────────  ────
        │          │       │        └─ index 3
        │          │       └─ starting pointer
        │          └─ element type (used to compute stride = sizeof(i32)=4)
        └─ result = %base + 3*4 bytes  =  &((i32*)base)[3]
                    ... but result type is just `ptr` (opaque)
```

### Control flow (terminators)

```llvm
ret i32 %v                          ; return a value
ret void                            ; return nothing
br label %target                    ; unconditional jump
br i1 %cond, label %t, label %f     ; conditional branch (2 successors)
switch i32 %x, label %def [ i32 0, label %a   i32 1, label %b ]
unreachable                         ; "control never gets here" (UB if it does)
```

### Comparisons (produce `i1`)

```llvm
%c = icmp eq  i32 %a, %b      ; ==   (ne, sgt, sge, slt, sle, ugt, uge, ult, ule)
%d = fcmp olt double %x, %y   ; ordered-less-than (o*=ordered, u*=unordered re: NaN)
```

### Calls and casts

```llvm
%r = call i32 @foo(i32 %a, ptr %b)   ; call a function
%w = zext  i8  %b to i32             ; zero-extend  (sext = sign-extend, trunc = narrow)
%f = sitofp i32 %i to double         ; int→float    (fptosi float→int, etc.)
%p = bitcast ptr %q to ptr           ; reinterpret bits (mostly no-op with opaque ptrs)
%i = ptrtoint ptr %p to i64          ; pointer ↔ integer
```

### The PHI node (the SSA superstar)

```llvm
%result = phi i32 [ %a, %then ], [ %b, %else ]
;                  └ value if we came from block %then
;                                 └ value if we came from block %else
```

A PHI "chooses" a value based on which predecessor block we arrived from. It's how SSA
expresses "a variable that has different values on different paths." Full treatment in the
next chapter — it deserves its own diagrams.

---

## 5. A complete, non-trivial example: `abs` with a branch

Watch all the pieces compose. C:

```c
int my_abs(int n) {
    if (n < 0) return -n;
    return n;
}
```

IR:

```llvm
define i32 @my_abs(i32 %n) {
entry:
  %isneg = icmp slt i32 %n, 0            ; i1: is n < 0 (signed)?
  br i1 %isneg, label %negate, label %done   ; two-way branch

negate:                                   ; predecessor of %done
  %neg = sub i32 0, %n                    ; 0 - n  = -n
  br label %done

done:                                     ; two predecessors: entry, negate
  %r = phi i32 [ %neg, %negate ], [ %n, %entry ]
  ret i32 %r
}
```

Its control-flow graph:

```
            ┌──────────┐
            │  entry   │  %isneg = icmp slt %n, 0
            └────┬─────┘
          true   │   false
        ┌────────┴────────┐
        ▼                 │
   ┌──────────┐           │
   │ negate   │ %neg=-n   │
   └────┬─────┘           │
        │                 │
        └────────┬────────┘
                 ▼
            ┌─────────┐
            │  done   │  %r = phi [ %neg, negate ], [ %n, entry ]
            └─────────┘  ret %r
```

Read the PHI: "if we reached `done` from `negate`, `%r = %neg`; if from `entry`, `%r = %n`."
That's a branchless way to express the conditional result in SSA. We'll see *why* SSA forces
this in the next chapter.

---

## 6. Data layout and target triple — the two strings that matter

```
  target triple =     "x86_64-apple-macosx14.0.0"
                      ───┬─── ──┬── ──────┬──────
                architecture  vendor  OS / version
```

The **triple** answers "who are we compiling for?" — it selects the backend, ABI, calling
conventions, and default sizes. Examples:

```
  x86_64-unknown-linux-gnu       64-bit x86 Linux
  aarch64-apple-darwin           Apple Silicon macOS
  riscv64-unknown-elf            bare-metal RISC-V
  wasm32-unknown-unknown         WebAssembly
```

The **datalayout** string encodes endianness, pointer size, and the alignment of each type:

```
  "e-m:o-p270:32:32-i64:64-f80:128-n8:16:32:64-S128"
    │   │     │       │      │       │           └ stack alignment 128 bits
    │   │     │       │      │       └ native integer widths
    │   │     │       │      └ x86_fp80 aligned to 128
    │   │     │       └ i64 aligned to 64
    │   │     └ pointers in addrspace 270 are 32-bit
    │   └ mangling: 'o' = Mach-O style
    └ 'e' = little-endian   ('E' = big-endian)
```

You rarely write these by hand: you get them from a `TargetMachine` (see
[../03-aot/02-target-machine-codegen.md](../03-aot/02-target-machine-codegen.md)). But IR
*must* be consistent with them, and the optimizer reads them (e.g., to know pointer size).

---

## 7. Constants, globals, linkage

```llvm
@g = global i32 0                ; external linkage, definition
@h = internal global i32 5       ; internal: not visible outside module (like C `static`)
@k = private constant [3 x i8] c"hi\00"   ; private: even the symbol is hidden
@p = external global i32         ; declared, defined elsewhere
```

Linkage controls symbol visibility to the linker. The common ones:

```
  external    visible everywhere; the default for defined functions/globals
  internal    visible only within this module (C `static`)
  private     like internal, but symbol name is dropped from the table entirely
  linkonce_odr  may be merged across modules (C++ inline functions/templates)
  weak        may be overridden by a strong definition
```

This matters a lot for both AOT (what the linker sees) and JIT (what symbols ORC must
resolve). Keep it in mind for sections 03 and 04.

---

## 8. How to *look at* IR while learning

Three habits that will accelerate you:

```bash
# 1. See what clang generates for any C snippet (great for "how do I express X in IR?")
>>> clang -O0 -S -emit-llvm snippet.c -o -        # prints .ll to stdout
>>> clang -O2 -S -emit-llvm snippet.c -o -        # see optimized version

# 2. Round-trip to verify your hand-written IR is valid
>>> llvm-as my.ll -o /dev/null                    # errors if invalid
>>> opt -verify -S my.ll -o -                      # run the verifier

# 3. Use Compiler Explorer (godbolt.org) mentally: source ↔ IR ↔ asm side by side
```

> **Pro tip:** the fastest way to learn "what IR does feature X map to" is to write the C,
> run `clang -O0 -emit-llvm -S`, and read the output. Then bump to `-O2` to see what the
> optimizer turns it into. This single workflow teaches you more IR than any reference.

---

## Mental model checkpoint

1. What are the top-level things a `Module` contains?
2. State the terminator rule for basic blocks in one sentence.
3. Why does `i32` not encode signedness, and where does signedness live instead?
4. With opaque pointers, where is the pointee type now specified?
5. What does `getelementptr` compute, and what does it *not* do?
6. Explain the PHI node in `%r = phi i32 [%neg,%negate],[%n,%entry]`.
7. What two strings tell LLVM "who and how" to compile for, and what does each encode?
8. Give the one-liner to dump clang's IR for a C file.

Next → [04-ssa-and-cfg.md](04-ssa-and-cfg.md)

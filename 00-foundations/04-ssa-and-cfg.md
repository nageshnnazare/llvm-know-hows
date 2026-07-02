# 00.04 · SSA, the CFG, Dominance, and PHI Nodes

> SSA (Static Single Assignment) is the single most important concept in modern compilers.
> It is *why* LLVM's optimizer is so powerful, and it explains the otherwise-baffling PHI
> node. Once SSA clicks, optimization passes stop being magic. We go slow and draw a lot.

---

## 1. The control-flow graph (CFG)

A function's basic blocks form a directed graph. Edges = possible control transfers
(branches). This is the **CFG**, and nearly every analysis walks it.

```c
int f(int n) {
    int s = 0;
    while (n > 0) { s += n; n -= 1; }
    return s;
}
```

```
        ┌──────────┐
        │  entry   │   s=0
        └────┬─────┘
             ▼
        ┌──────────┐ ◀───────────┐
        │  cond    │ n > 0 ?     │
        └────┬─────┘             │
       true  │   false           │
        ┌────┴─────┐             │
        ▼          ▼             │
   ┌───────────┐  ┌──────────┐   │
   │  body     │  │  exit    │   │
   │ s+=n;n-=1 │  │ return s │   │
   └────┬──────┘  └──────────┘   │
        └────────────────────────┘   (back-edge: body → cond)
```

- A **back-edge** (body→cond) is what makes this a *loop*.
- `entry` is the unique **entry block** (no predecessors).
- `exit` here is a block with no successors (it ends in `ret`).

The CFG is *the* substrate for optimization: dead-code elimination removes unreachable
nodes, loop optimizations recognize back-edges, etc.

---

## 2. What "SSA" means

**Static Single Assignment**: every variable (value) is **assigned exactly once**, textually.
There is no reassignment. If the source reassigns a variable, SSA invents a *new* version.

```
  SOURCE (mutable)            SSA (each name assigned once)
  ────────────────           ──────────────────────────────
  x = 1                       x1 = 1
  x = x + 2                   x2 = x1 + 2
  x = x * 3                   x3 = x2 * 3
  use(x)                      use(x3)
```

Why on earth do this? Because it makes **def-use chains trivial and exact**:

```
   In SSA, the name x3 IS its definition. To find "where does this value come
   from?" you just look at x3's single defining instruction. To find "who uses
   this value?" you list x3's uses. No aliasing, no "which assignment reaches
   here?" puzzle. Data flow becomes a graph you can read directly.
```

```
        def ──────────▶ use
         x2 = x1 + 2          (the ONLY def of x2)
            ╲
             ╲──────▶ use      every use points back to exactly one def
                               every def is reached by exactly one assignment
```

Optimizations like constant propagation, common-subexpression elimination, and dead-code
elimination become almost mechanical on SSA, because "the value flowing here" is
unambiguous.

---

## 3. The problem SSA hits at merge points — and the PHI solution

SSA says "assign once." But what about a value that depends on *which path* you took?

```c
int x;
if (c) x = 10;
else   x = 20;
return x;          // which x? depends on the branch!
```

Naively in SSA:

```
   if-branch:  x1 = 10
   else-branch: x2 = 20
   merge:       return ???   ← x1 or x2? We can't pick statically!
```

The merge block needs a value that is "x1 if we came from the if-branch, x2 if from the
else-branch." That is *exactly* the **PHI node**:

```
            ┌──────────┐
            │  entry   │ br i1 %c, then, else
            └────┬─────┘
          true   │   false
        ┌────────┴────────┐
        ▼                 ▼
   ┌──────────┐      ┌──────────┐
   │  then    │      │  else    │
   │ x1 = 10  │      │ x2 = 20  │
   └────┬─────┘      └────┬─────┘
        └────────┬────────┘
                 ▼
            ┌──────────────────────────────────────┐
            │ merge                                │
            │ x3 = phi i32 [10, then], [20, else]  │
            │ return x3                            │
            └──────────────────────────────────────┘
```

```
  x3 = phi i32 [ 10, %then ], [ 20, %else ]
               └───┬───┘       └───┬───┘
       "if control arrived       "if it arrived
        from %then, x3 = 10"      from %else, x3 = 20"
```

**Rules of PHI nodes** (memorize — the verifier enforces them):

```
  ┌──────────────────────────────────────────────────────────────────────────┐
  │ 1. PHIs must be the FIRST instructions in a block (before any non-PHI).  │
  │ 2. A PHI must have exactly one [value, predecessor] pair PER predecessor.│
  │ 3. The value is "chosen" based on which edge control came in on.         │
  │ 4. PHIs execute "simultaneously" / atomically at block entry — they all  │
  │    read the OLD values, conceptually, before any take effect.            │
  └──────────────────────────────────────────────────────────────────────────┘
```

> **The mental model:** a PHI is not a runtime computation in the usual sense; it's the SSA
> bookkeeping device that says "this value's source depends on the incoming edge." After
> register allocation, PHIs become copies/moves on the edges — but in IR they keep SSA clean.

---

## 4. Dominance: the backbone of correctness

To know *which* definitions are valid to use at a point, and to *place* PHIs correctly, we
need **dominance**.

```
   Block A DOMINATES block B  ⟺  EVERY path from entry to B passes through A.
```

```
        ┌──────────┐
        │  entry   │
        └────┬─────┘
             ▼
        ┌────────┐
        │    A   │   A dominates B, C, D, E (all paths go through A)
        └────┬───┘
        ┌────┴────┐
        ▼         ▼
   ┌──────┐   ┌──────┐
   │  B   │   │  C   │   B does NOT dominate D (you can reach D via C)
   └──┬───┘   └──┬───┘
      └────┬─────┘
           ▼
        ┌──────┐
        │  D   │   D dominates E
        └──┬───┘
           ▼
        ┌──────┐
        │  E   │
        └──────┘
```

Key facts:

- **The SSA dominance property:** *a definition must dominate all its uses.* You can only use
  `%x` at a point if `%x`'s definition is guaranteed to have executed — i.e., dominates that
  point. The verifier rejects IR that violates this. (PHIs are the escape hatch: their
  operands need only dominate the *predecessor edges*, not the PHI's own block.)

- **Dominator tree:** the dominance relation forms a tree (each block's immediate dominator
  is its parent). Many passes walk this tree.

```
   Dominator tree for the CFG above:
        entry
          │
          A
        ┌─┼─┐
        B C  D
             │
             E
```

- **Dominance frontier:** the set of blocks where a definition in block X "stops
  dominating" — precisely where PHIs for X must be inserted. This is the engine of the
  classic SSA-construction algorithm (Cytron et al.).

You won't implement dominator computation (LLVM provides `DominatorTree`), but knowing the
*rule* — "defs dominate uses" — explains a huge fraction of "why is my IR invalid?" errors.

---

## 5. How SSA is actually built: `alloca` + mem2reg

Here's a practical secret that makes frontend codegen *much* easier. **You do not have to
generate SSA directly.** Generating PHIs by hand is painful. Instead, the standard trick:

```
  STEP 1 (frontend, easy):           STEP 2 (LLVM pass mem2reg, automatic):
  put every mutable variable          promote allocas to SSA registers,
  in stack memory via alloca;         inserting PHIs where needed.
  use load/store to access it.

  ──────────────────────────         ─────────────────────────────────
  %x = alloca i32                     ; (memory gone)
  store i32 10, ptr %x      ──────▶   %x3 = phi i32 [10, then],[20, else]
  ...                                 ; clean SSA, no loads/stores
  %v = load i32, ptr %x
```

So the recipe every frontend uses:

```
   ┌──────────────────────────────────────────────────────────────────┐
   │ 1. For each local variable, emit `alloca` in the entry block.    │
   │ 2. Reads → `load`, writes → `store`. NO PHIs by hand.            │
   │ 3. Run the `mem2reg` (PromoteMemoryToRegister) pass.             │
   │    It deletes the allocas and builds correct SSA + PHIs for you. │
   └──────────────────────────────────────────────────────────────────┘
```

This is *the* reason the Kaleidoscope tutorial and real frontends generate allocas
liberally and lean on `mem2reg`. We use this exact approach in
[../01-frontend/03-ast-to-ir.md](../01-frontend/03-ast-to-ir.md).

Concrete before/after:

```llvm
; ── BEFORE mem2reg: frontend output (allocas everywhere) ──
define i32 @pick(i1 %c) {
entry:
  %x = alloca i32
  br i1 %c, label %t, label %e
t:
  store i32 10, ptr %x
  br label %m
e:
  store i32 20, ptr %x
  br label %m
m:
  %v = load i32, ptr %x
  ret i32 %v
}

; ── AFTER mem2reg: clean SSA with a PHI ──
define i32 @pick(i1 %c) {
entry:
  br i1 %c, label %t, label %e
t:
  br label %m
e:
  br label %m
m:
  %v = phi i32 [ 10, %t ], [ 20, %e ]
  ret i32 %v
}
```

Notice mem2reg figured out it needed a PHI in `m` and built it. You wrote zero PHIs.

---

## 6. Why optimizers love SSA — a worked propagation

Watch constant propagation + dead code elimination flow through SSA in one pass each:

```
  start                  after constant prop        after dead-code elim
  ─────────              ──────────────────         ────────────────────
  %a = add i32 2, 3      %a = 5  (folded)            (a,b removed; unused)
  %b = mul i32 %a, 4     %b = mul i32 5, 4 = 20      ret i32 20
  %c = sub i32 %b, %b    %c = sub i32 20, 20 = 0
  ret i32 %b             ret i32 20
```

Because each `%name` has exactly one definition, "replace all uses of `%a` with 5" is a safe,
local rewrite — no need to prove no intervening reassignment (there can't be one!). That
safety is what makes the optimizer both fast and aggressive.

---

## Mental model checkpoint

1. What is a back-edge, and what does its presence indicate?
2. State the SSA invariant in one sentence.
3. Why does SSA make def-use chains exact?
4. What problem do PHI nodes solve, and where in a block must they appear?
5. Define "A dominates B."
6. State the dominance rule that every valid use of a value must satisfy.
7. Describe the alloca + mem2reg strategy and why frontends use it.
8. Why is "replace all uses of %a with a constant" trivially safe in SSA?

Next → [05-environment-setup.md](05-environment-setup.md)

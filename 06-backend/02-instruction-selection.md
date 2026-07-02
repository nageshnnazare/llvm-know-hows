# 06.02 · Instruction Selection — SelectionDAG & GlobalISel

> Instruction selection is where target-independent IR becomes target-specific machine
> instructions. It's the most intricate part of the backend. We dig into how SelectionDAG
> matches patterns, how targets *describe* their instructions in TableGen, and how the newer
> GlobalISel differs.

---

## 1. The fundamental problem

```
   Given:   target-independent IR operation(s)
   Find:    the cheapest sequence of THIS target's instructions that computes
            the same result, respecting the target's registers and constraints.

   It's a COVERING problem: tile the computation DAG with available
   instruction "patterns," minimizing total cost.
```

```
   computation:  result = (a * b) + c
                       (add (mul a b) c)

   Target A (has multiply-accumulate):       Target B (no madd):
   ┌────────────────────────────────┐         ┌──────────────────────────────┐
   │ cover whole tree with ONE op:  │         │ cover with TWO ops:          │
   │   MLA r, a, b, c               │         │   MUL t, a, b                │
   │                                │         │   ADD r, t, c                │
   └────────────────────────────────┘         └──────────────────────────────┘
   Same IR, different coverings → different (optimal) instruction sequences.
```

---

## 2. SelectionDAG in detail

For each basic block, LLVM builds a **DAG** (directed acyclic graph) where nodes are
operations and edges are data (and chain) dependencies.

```
   IR block:
     %0 = load i32, ptr %p
     %1 = mul i32 %0, 4
     store i32 %1, ptr %q

   SelectionDAG (data edges ──, chain edges for ordering ╌╌):
                                 [EntryToken]
                                      ╎ (chain)
              ┌──────────── (load %p) ╌╌╌╌╌╌╌╌╌┐
              │ value                          ╎ chain
              ▼                                ▼
          (mul, 4)                        (store) ◀── needs %1
              │                               ╎
              └───────────────────────────────┘
   Chain edges enforce memory ordering (the load must precede the store);
   data edges carry SSA values.
```

The four SelectionDAG phases (recall chapter 06.01 §2), now elaborated:

```
  ┌─────────────────┬──────────────────────────────────────────────────────────┐
  │ Build           │ IR → initial "illegal" DAG (uses generic ISD nodes).     │
  │ Legalize types  │ make every value type natively supported: split (i128 →  │
  │                 │ 2×i64), promote (i1 → i8), expand, scalarize vectors.    │
  │ Legalize ops    │ make every operation supported: unsupported op → expand  │
  │                 │ into supported ops, or a runtime LIBCALL (e.g. __divdi3).│
  │ Combine         │ DAG-level peephole (fold, simplify) — runs a few times.  │
  │ Select          │ match DAG patterns → target nodes (the actual ISel).     │
  │ Schedule        │ linearize DAG → ordered MachineInstrs.                   │
  └─────────────────┴──────────────────────────────────────────────────────────┘
```

```
   Legalization examples you can SEE:
     i64 add on a 32-bit target  → ADD + ADC (add-with-carry) pair
     frem (float remainder)       → call to fmodf  (a libcall — no HW op)
     <8 x i32> on SSE2 (4-wide)  → two <4 x i32> ops (vector splitting)
```

---

## 3. How targets describe instructions: TableGen patterns

The magic of "the same IR → different instructions per target" is that each backend
*declares its instructions as patterns* in `.td` files. `llvm-tblgen` compiles these into a
matcher.

```tablegen
// Simplified: an x86 32-bit register-register add.
def ADD32rr : I<0x01, MRMDestReg, (outs GR32:$dst),
                (ins GR32:$src1, GR32:$src2),
                "add{l}\t{$src2, $dst|$dst, $src2}",
                // THE PATTERN: this instruction implements (add src1, src2)
                [(set GR32:$dst, (add GR32:$src1, GR32:$src2))]>;
                 ───────────────────────────────────────────
                 "when you see an `add` of two GR32 values producing a GR32,
                  you MAY select this ADD32rr instruction."
```

```
   ┌──────────────────────────────────────────────────────────────────────────┐
   │ The bracketed [(set $dst, (add $src1, $src2))] is a DAG PATTERN. The     │
   │ instruction-selection table is BUILT from all such patterns across the   │
   │ target. ISel walks the input DAG and finds matching patterns, preferring │
   │ those that cover more nodes at lower cost. Add an instruction = add a    │
   │ pattern; no hand-written matcher needed.                                 │
   └──────────────────────────────────────────────────────────────────────────┘
```

Complex patterns cover subtrees — this is how multiply-add, load-and-op, and addressing
modes get matched as single instructions:

```tablegen
   // A fused multiply-add pattern covering (add (mul a b) c):
   [(set RegClass:$dst, (add (mul RegClass:$a, RegClass:$b), RegClass:$c))]
   // matches the WHOLE subtree → one MADD/FMA instruction.

   // x86 complex addressing: (load (add base (shl index, scale)))
   //  → a single mov with a [base + index*scale] memory operand.
```

---

## 4. Selection DAG → MachineInstr

After matching, the DAG nodes are target instructions; scheduling linearizes them into
`MachineInstr`s with virtual registers:

```
   selected DAG (target nodes)        →     MachineInstr stream (virtual regs)
   ─────────────────────────                ──────────────────────────────────
   (X86::MOV32rm %p)                        %0:gr32 = MOV32rm %p, ...
   (X86::SHL32ri %0, 2)                     %1:gr32 = SHL32ri %0, 2
   (X86::MOV32mr %q, %1)                    MOV32mr %q, ..., %1
   note: still VIRTUAL registers (%0, %1) — regalloc (next chapter) assigns real ones.
```

---

## 5. GlobalISel — the alternative pipeline

SelectionDAG works per-basic-block and builds a separate DAG data structure, which costs
compile time. **GlobalISel** operates on the whole function directly in MachineIR, in four
passes, no separate DAG:

```
   GlobalISel pipeline:
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ IRTranslator   : LLVM IR → generic MachineInstr (gMIR), virtual regs    │
   │ Legalizer      : make gMIR ops legal for the target (like DAG legalize) │
   │ RegBankSelect  : assign each vreg to a register BANK (e.g. GPR vs FPR)  │
   │ InstructionSelect: gMIR → real target MachineInstrs (pattern match)     │
   └─────────────────────────────────────────────────────────────────────────┘
```

```
   SelectionDAG                       GlobalISel
   ─────────────────────────          ─────────────────────────────────
   per-basic-block                    whole-function (global)
   builds a separate DAG              works in MIR directly (no DAG)
   mature, best codegen at -O2+       faster compile, improving quality
   default on most targets            default at -O0 on AArch64; opt-in else
   reuses TableGen patterns ─────────▶ ALSO reuses many TableGen patterns
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ Why GlobalISel? SelectionDAG's per-block DAG construction is slow and   │
   │ loses cross-block context. GlobalISel is faster (good for -O0 / JIT     │
   │ baseline tiers) and global. It's gradually replacing SelectionDAG, but  │
   │ SelectionDAG still wins on peak -O2/-O3 quality on most targets today.  │
   └─────────────────────────────────────────────────────────────────────────┘
```

GlobalISel's relevance to *this guide*: a JIT baseline tier (chapter 04.04) wants fast
compiles, so GlobalISel (or fast SelectionDAG mode) is a natural fit, with optimizing tiers
using full SelectionDAG.

---

## 6. Seeing it happen

```bash
# Watch SelectionDAG build/legalize/select (debug LLVM build):
>>> llc -debug-only=isel -O2 in.ll -o /dev/null 2> isel.log
>>> llc -view-dag-combine1-dags in.ll    # DAG after first combine (graphviz)
>>> llc -view-sched-dags in.ll           # DAG handed to the scheduler

# Use GlobalISel instead of SelectionDAG (where supported):
>>> llc -global-isel in.ll -o out.s

# Stop right after selection to see virtual-register MachineIR:
>>> llc -stop-after=finalize-isel in.ll -o selected.mir
```

Reading `selected.mir` shows target opcodes with virtual registers — the exact handoff to
register allocation.

---

## Mental model checkpoint

1. Frame instruction selection as a covering problem.
2. What do data edges vs chain edges represent in a SelectionDAG?
3. Distinguish "legalize types" from "legalize operations," with an example of each.
4. What is a libcall and when does legalization emit one?
5. How does a target describe an instruction so ISel can pick it, and what is a DAG pattern?
6. How can a single instruction match a whole subtree (give an example)?
7. List GlobalISel's four passes and how the pipeline differs from SelectionDAG.
8. Why might a JIT's baseline tier prefer GlobalISel?

Next → [03-register-allocation.md](03-register-allocation.md)

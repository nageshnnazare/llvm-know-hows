# 06.03 · Register Allocation

> After instruction selection, code uses unlimited *virtual* registers. Real CPUs have a
> handful of *physical* registers. Register allocation is the (NP-hard) problem of mapping
> one to the other, spilling to memory when necessary. It's one of the most performance-
> critical backend stages. We build intuition with live ranges, interference, and LLVM's
> greedy allocator.

---

## 1. The problem

```
   ISel output: infinite virtual registers
     %v0 = ...    %v1 = ...    %v2 = ...   ... %v99 = ...

   Reality (x86-64 integer regs): ~16 physical registers
     rax rbx rcx rdx rsi rdi rbp rsp r8 r9 r10 r11 r12 r13 r14 r15
     (and some are reserved: rsp = stack pointer, etc.)

   GOAL: assign each virtual reg a physical reg such that two virtual regs
   that are "alive at the same time" never share a physical reg. When you
   run out, SPILL: keep some value in memory, load it just before use.
```

```
   ┌────────────────────────────────────────────────────────────────────────┐
   │ Bad allocation = lots of spills = lots of loads/stores = slow code.    │
   │ Register allocation quality is often the single biggest factor in      │
   │ -O0 vs -O2 performance. This is where "use the stack" becomes "use a   │
   │ register" — the inverse of the frontend's alloca trick (ch.00.04).     │
   └────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Live ranges and liveness

A virtual register is **live** from its definition to its last use. Liveness analysis (an IR/
MIR analysis) computes these ranges.

```
   instr               %a   %b   %c
   ─────────────────   ───  ───  ───
   %a = ...            ●def
   %b = ...            │    ●def
   %c = add %a, %b     │use │use ●def      ← %a, %b, %c all live here
   ...= %c             │    │    │use
                       ▼    ▼    ▼
   live ranges:       [a]  [b]  [c]
                      a and b OVERLAP (both live at the add) → they INTERFERE
                      c starts where a,b end → c can REUSE a's or b's register
```

```
   LIVE RANGE: the set of program points where a value must be retained.
   Two values INTERFERE if their live ranges overlap (both needed simultaneously);
   interfering values cannot share a physical register.
```

---

## 3. The interference graph (the classic model)

Model registers as **graph coloring**: nodes = virtual regs, edges = interference, colors =
physical registers. A valid allocation = a proper coloring with K colors (K = #physical
regs).

```
   live ranges:  a──b overlap, b──c overlap, a──c do NOT overlap

   interference graph:        coloring with 2 registers (R1, R2):
        a ─── b                   a = R1
              │                   b = R2   (b interferes with a → different)
              c                   c = R1   (c interferes with b, NOT a → reuse R1)

   2 colors suffice here. If the graph needed 3 colors but we have 2 regs,
   we must SPILL one node to memory to make it colorable.
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ Graph coloring with K colors is NP-hard in general, so allocators use   │
   │ HEURISTICS. Chaitin-Briggs (classic) builds the interference graph,     │
   │ simplifies it (remove low-degree nodes), and spills when stuck. LLVM's  │
   │ default is a different, faster approach: the Greedy allocator.          │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Spilling

When there aren't enough registers, **spill** a value: store it to a stack slot after it's
computed, and reload it right before each use. Costly, so allocators spill the *cheapest*
(least-used, coldest) values.

```
   before (want %v in a reg but none free):
       %v = expensive_compute
       ... many other live values ...
       use %v

   after spilling %v:
       %v = expensive_compute
       store %v -> [stack_slot]        ◀ spill (write to memory)
       ... other values use the freed register ...
       %v.reload = load [stack_slot]    ◀ reload before use
       use %v.reload
```

```
   Spill cost heuristic ≈ (number of uses × loop depth weight) / live-range size.
   Spill values that are used rarely and span a long range; KEEP in registers
   the hot, frequently-used values (especially inside loops).
```

---

## 5. LLVM's allocators

```
  ┌──────────────────────┬─────────────────────────────────────────────────────┐
  │ Fast (regallocfast)  │ -O0. Allocates per-basic-block, minimal analysis.   │
  │                      │ Fast compile, poor code. Good for JIT baseline tier.│
  ├──────────────────────┼─────────────────────────────────────────────────────┤
  │ Greedy (default)     │ -O1+. Global, priority-based: allocates large/hot   │
  │                      │ live ranges first, splits ranges, evicts & re-tries,│
  │                      │ spills as last resort. The production workhorse.    │
  ├──────────────────────┼─────────────────────────────────────────────────────┤
  │ Basic / PBQP         │ alternative/experimental allocators (PBQP solves a  │
  │                      │ partitioned quadratic problem; niche).              │
  └──────────────────────┴─────────────────────────────────────────────────────┘
```

The Greedy allocator's loop, conceptually:

```
   priority queue of live ranges (largest/hottest first)
       │
   pop a live range LR:
       try to assign a free physical reg
         │ success → done
         │ no free reg →
       can we EVICT a lower-priority range from a reg? → evict it (it re-queues)
         │ no good eviction →
       can we SPLIT LR into smaller pieces that fit? → split, re-queue pieces
         │ no →
       SPILL LR to the stack.
   repeat until queue empty.
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ "Live-range splitting" is Greedy's key trick: instead of spilling a     │
   │ whole value, split its live range so part lives in a register (where    │
   │ it's hot) and part in memory (where it's cold). Finer-grained than      │
   │ all-or-nothing coloring → better code.                                  │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 6. Register classes and constraints

Not all registers are interchangeable. Targets define **register classes** and instructions
constrain which class their operands use.

```
   x86 register classes (examples):
     GR32   : 32-bit general regs (eax, ecx, ...)
     GR64   : 64-bit general regs (rax, rcx, ...)
     FR64   : xmm regs for doubles
     ...
   An instruction like ADD32rr requires GR32 operands; a float op requires FR*.
   The allocator must respect the class: you can't put a double in eax.

   Plus hard constraints:
     • Fixed regs: x86 division uses eax/edx specifically; calls pass args in
       rdi,rsi,... per the ABI; the allocator must honor these.
     • Callee-saved vs caller-saved: affects prologue/epilogue (ch.06.01 §5).
     • Two-address ops: some x86 ops overwrite an operand (dst = dst op src).
```

---

## 7. Coalescing

A frequent opportunity: ISel emits lots of `copy` instructions (e.g., from PHI elimination —
recall PHIs become copies on edges, chapter 00.04 §3). **Coalescing** assigns the source and
destination of a copy the *same* register, deleting the copy.

```
   before:  %v1 = COPY %v0        (if %v0, %v1 don't interfere)
            use %v1
   after coalescing: %v0 and %v1 share a register → COPY deleted
            use %v0

   PHI elimination inserts copies on CFG edges; coalescing removes most of
   them, which is why well-allocated code has few register moves.
```

```
   The tension: coalescing %v0 and %v1 MERGES their live ranges, which can
   make the merged range harder to color (more interference). Allocators
   coalesce conservatively to avoid causing new spills.
```

---

## 8. Seeing register allocation

```bash
# MIR before allocation (virtual regs) vs after (physical regs):
>>> llc -stop-before=greedy in.ll -o pre-ra.mir     # %0, %1 virtual
>>> llc -stop-after=greedy  in.ll -o post-ra.mir    # eax, ecx physical

# Force the fast allocator (see worse code, like -O0 / JIT baseline):
>>> llc -regalloc=fast in.ll -o fast.s
>>> llc -regalloc=greedy -O2 in.ll -o greedy.s
>>> diff fast.s greedy.s        # compare spill counts & quality

# Spill statistics:
>>> llc -O2 -stats in.ll -o /dev/null 2>&1 | grep -i spill
```

Diffing `pre-ra.mir` and `post-ra.mir` is the single most illuminating exercise: you watch
virtual registers collapse onto the real register file, and see spills appear when pressure
is high.

---

## 9. Why this matters for AOT and JIT

```
   AOT (-O2): use Greedy — spend compile time for the best register use,
              since you compile once, offline.
   JIT baseline tier: use Fast — minimize compile latency; accept spills,
              because this code may be replaced by an optimized tier anyway.
   JIT optimizing tier: use Greedy — the hot code deserves great allocation.
```

This mirrors the tiering theme from chapter 04.04: **register allocation is one of the knobs
you trade between compile speed and code speed.** The same `TargetMachine`, different
allocator, different point on the curve.

---

## Mental model checkpoint

1. State the register allocation problem in one sentence.
2. What is a live range, and when do two values interfere?
3. How is allocation modeled as graph coloring, and what do nodes/edges/colors represent?
4. What is a spill, and which values should an allocator prefer to spill?
5. Describe the Greedy allocator's assign/evict/split/spill loop.
6. What is live-range splitting and why is it better than whole-value spilling?
7. What are register classes and two kinds of hard constraints the allocator must respect?
8. What is coalescing, where do the copies come from, and what's the risk of over-coalescing?
9. Which allocator fits a JIT baseline tier vs an AOT `-O2` build, and why?

Backend deep dive complete. Next, the capstone → [../07-capstone/01-one-language-three-backends.md](../07-capstone/01-one-language-three-backends.md)

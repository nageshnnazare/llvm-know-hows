# 06.01 · The Backend Codegen Pipeline

> Sections 03/04 treated the backend as a black box ("`TargetMachine` turns IR into bytes").
> Now we open it. The backend is itself a pipeline: LLVM IR → SelectionDAG → MachineInstr →
> MC → bytes. Understanding it explains *why* certain IR compiles the way it does, and is
> essential if you ever add a target or debug bad codegen.

---

## 1. The journey from IR to bytes

```
   LLVM IR (target-independent, SSA, virtual values)
        │  ① Instruction Selection (ISel): match IR → target machine ops
        ▼
   SelectionDAG  (or GlobalISel path)     — per basic block DAG of target ops
        │  ② Scheduling: order DAG nodes into a linear list
        ▼
   MachineInstr (MIR), VIRTUAL registers  — target ops, infinite virtual regs
        │  ③ Register Allocation: map virtual regs → physical regs (+spills)
        ▼
   MachineInstr (MIR), PHYSICAL registers
        │  ④ Prologue/Epilogue, frame finalization, late peepholes
        ▼
   MachineInstr (final)
        │  ⑤ MC Layer: encode each instruction to bytes; build sections/relocs
        ▼
   object file (.o)  or  bytes in memory (JIT)
```

Each representation is *lower* than the last (recall the abstraction-level diagram from
chapter 00.02). The backend's job is to commit, step by step, to a specific machine.

```
   target-INDEPENDENT  ────────────────────────────▶  target-SPECIFIC
   LLVM IR        SelectionDAG       MachineInstr           MC/bytes
   "add i32"      "X86 ADD32rr"      "%vreg0 = ADD32rr"     0x01 0xC8...
   virtual        target opcode      virtual regs           real bytes
```

---

## 2. Stage ① — Instruction Selection (the big one)

ISel translates target-independent IR operations into target machine instructions. LLVM has
two frameworks:

```
  ┌─────────────────────┬──────────────────────────────────────────────────────┐
  │ SelectionDAG        │ the mature, default path. Builds a per-block DAG of  │
  │                     │ operations, legalizes it, then pattern-matches DAG   │
  │                     │ subtrees to machine instructions. High quality.      │
  ├─────────────────────┼──────────────────────────────────────────────────────┤
  │ GlobalISel          │ newer, operates on a global (whole-function) basis   │
  │                     │ without building a separate DAG; faster compile, used│
  │                     │ at -O0 on some targets (esp. AArch64). Covered next  │
  │                     │ chapter.                                             │
  └─────────────────────┴──────────────────────────────────────────────────────┘
```

The SelectionDAG idea, visualized for `a*b + c`:

```
   IR:  %m = mul i32 %a, %b
        %r = add i32 %m, %c

   DAG (data flows UP to the root):
                  (add)
                 ╱     ╲
              (mul)    [c]
             ╱    ╲
          [a]      [b]

   Pattern matching: the target may have a single "multiply-add" instruction.
   ISel matches the (add (mul a b) c) SUBTREE and emits ONE machine op:
                  X86: (no fused int madd, so) IMUL + ADD
                  ARM: MLA  Rd, Ra, Rb, Rc      ◀ one instruction for the whole tree!
```

```
   ┌──────────────────────────────────────────────────────────────────────────┐
   │ ISel = tree/DAG pattern matching. The target describes its instructions  │
   │ as PATTERNS over IR/DAG operations (in TableGen .td files). ISel covers  │
   │ the DAG with the lowest-cost set of patterns. This is why "the same IR   │
   │ becomes different instructions on different CPUs" — different patterns.  │
   └──────────────────────────────────────────────────────────────────────────┘
```

SelectionDAG's internal sub-phases:

```
   build DAG ─▶ legalize types (e.g. i64 on 32-bit → split into i32 pairs)
             ─▶ legalize operations (unsupported ops → libcalls or expansions)
             ─▶ DAG combine (peephole on the DAG)
             ─▶ select (match patterns → machine ops)
             ─▶ schedule (linearize the DAG into an instruction order)
```

"Legalization" is key: it rewrites operations the target *can't* do natively into ones it
can (e.g., a 128-bit add → several 64-bit adds with carry; a float divide → a library call).

---

## 3. Stage ② — Scheduling

The DAG expresses *data dependencies*, not an order. Scheduling picks a linear order that
respects dependencies while minimizing stalls (using the target's latency/port model).

```
   DAG (partial order)            Scheduled (total order)
   ──────────────────             ────────────────────────
   load A ─┐                      load A      ; start early (high latency)
   load B ─┤ both feed add        load B      ; overlap with A's latency
           ▼                      add  A, B   ; now both ready
          add                     ...
   Goal: hide latencies, keep execution ports busy, reduce register pressure.
```

This is where the `TargetMachine`'s scheduling model (instruction latencies, throughput)
earns its keep. At `-O0` scheduling is minimal (fast compile); at `-O2+` it's careful.

---

## 4. Stage ③ — Register Allocation (its own chapter)

After ISel/scheduling, code uses **infinite virtual registers**. Real CPUs have a fixed,
small set. Register allocation maps virtual → physical, inserting **spills** (store to stack,
reload later) when there aren't enough. This is so important it gets
[03-register-allocation.md](03-register-allocation.md) to itself.

```
   before:  %vreg0 = ADD %vreg1, %vreg2     (unlimited virtual regs)
   after:   eax    = ADD ecx, edx           (mapped to physical regs)
            ...or if no reg free: spill some vreg to [rsp+8], reload before use.
```

---

## 5. Stage ④ — Frame finalization, prologue/epilogue

Now that we know which physical registers and how much stack are used, the backend inserts
function prologue/epilogue and resolves stack-relative addresses.

```
   prologue (entry):  push rbp; mov rbp, rsp; sub rsp, <framesize>
                      (save callee-saved regs used; set up frame pointer)
   ... body ...
   epilogue (return): add rsp, <framesize>; pop rbp; ret
                      (restore callee-saved regs; tear down frame)
```

```
   This is where ABI/calling-convention details (which regs are callee-saved,
   how args/return are passed — recall ch.03.04's runtime) get baked into code.
   The TargetMachine knows the convention for the triple.
```

---

## 6. Stage ⑤ — The MC layer: instructions become bytes

The **MC** (Machine Code) layer is LLVM's assembler/disassembler core. It takes final
`MachineInstr`s and encodes them into actual bytes, organizing them into sections with
symbols and relocations (exactly the object-file structure from chapter 03.04).

```
   MachineInstr: ADD32rr eax, ecx
        │  MCInst (abstract encoded instruction)
        ▼
   MCCodeEmitter: encode to bytes  →  0x01 0xC8
        │  MCStreamer writes into sections
        ▼
   .text: [...0x01 0xC8...]   .symtab: [main, ...]   .rela.text: [...]
        │
        ▼
   ObjectWriter (ELF/Mach-O/COFF) → .o file
   ── or ── (JIT) the bytes are kept in memory for ORC to link (section 04)
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ The MC layer is shared by the assembler (llvm-mc), the integrated       │
   │ assembler in the compiler, AND the JIT. AOT writes its output to a      │
   │ file; the JIT keeps it in memory — SAME MC layer, different sink.       │
   │ This is, again, "AOT and JIT share the backend" made concrete.          │
   └─────────────────────────────────────────────────────────────────────────┘
```

---

## 7. Watching the pipeline with tools

You can stop the backend at each stage and inspect it — the best way to build intuition:

```bash
# See the SelectionDAG (requires a debug build of LLVM):
>>> llc -view-isel-dags in.ll          # opens a graph of the DAG
>>> llc -debug-only=isel in.ll         # textual ISel trace

# Dump MachineIR (MIR) before/after register allocation:
>>> llc -stop-after=finalize-isel in.ll -o before-ra.mir   # virtual regs
>>> llc -stop-before=regallocfast    in.ll -o pre-ra.mir
>>> llc -print-after-all in.ll 2> backend-trace.txt        # firehose

# Final assembly and object:
>>> llc in.ll -o out.s                 # assembly
>>> llc -filetype=obj in.ll -o out.o   # object
>>> llvm-objdump -d out.o              # disassemble to confirm
```

```
   -stop-after / -stop-before let you snapshot MIR at any pipeline point.
   Reading the MIR before vs after register allocation makes stage ③ tangible:
   you literally see %vreg names turn into eax/ecx/etc.
```

---

## 8. How this connects to AOT and JIT

You've now seen what `TargetMachine::addPassesToEmitFile` (chapter 03.02) actually builds:
stages ①–⑤. And the JIT's `IRCompileLayer` (chapter 04.02) runs the *same* stages, sending
the MC output to memory instead of a file.

```
   addPassesToEmitFile (AOT)  ─┐
                               ├─▶ builds the SAME stage ①–⑤ pipeline
   IRCompileLayer (JIT)       ─┘    on the SAME TargetMachine
                                    only the final sink differs (.o vs memory)
```

That symmetry — one backend, two sinks — is the structural payoff of LLVM's design and the
thread that ties this whole guide together.

---

## Mental model checkpoint

1. List the five backend stages from LLVM IR to object bytes, naming the IR form at each.
2. What does instruction selection do, and why does the same IR yield different instructions
   on different targets?
3. What is "legalization" and give two examples.
4. What does the scheduler optimize for, using what target model?
5. Why is register allocation necessary, and what is a "spill"?
6. What does the prologue/epilogue set up, and what target knowledge informs it?
7. What is the MC layer, and how is it shared between AOT, JIT, and `llvm-mc`?
8. Which `llc` flags let you inspect MIR before and after register allocation?

Next → [02-instruction-selection.md](02-instruction-selection.md)

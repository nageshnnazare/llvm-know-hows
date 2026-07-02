# 03.04 · Object Files, Relocations, Linking & the Runtime

> The `.o` → executable step is mysterious until you understand *object files* and
> *relocations*. This chapter demystifies them — it's also the conceptual key to JIT, because
> a JIT does linking *in memory at runtime*. Master this and ORC's "linker" makes sense.

---

## 1. Why object files aren't runnable yet

When the backend emits `square.o`, it doesn't yet know:

```
   • The final ADDRESS where the code will live in memory.
   • The addresses of symbols defined in OTHER objects/libraries (printf, sin).
```

So it leaves **holes** — placeholders — plus a list of instructions for how to fill them.
Those instructions are **relocations**. An object file is "machine code with blanks and a
to-do list."

```
   ┌──────────────── square.o (relocatable) ──────────────────┐
   │ .text  (code)                                            │
   │    square:  fmul ...                                     │
   │    main:    call <????>   ◀── address of printf unknown! │
   │ .rodata     "%f\n\0"                                     │
   │ symbol table:  square (defined), main (defined),         │
   │                printf (UNDEFINED — someone else has it)  │
   │ relocations:   at offset 0x12 in .text, patch with the   │
   │                address of `printf` (type R_X86_64_PLT32) │
   └──────────────────────────────────────────────────────────┘
```

---

## 2. Anatomy of an object file (ELF as the example)

ELF (Linux), Mach-O (macOS), and COFF (Windows) differ in detail but share this structure:

```
   ┌────────────────────────────────────────────────────────────────────┐
   │ HEADER         arch, type (relocatable/exec), entry point          │
   ├────────────────────────────────────────────────────────────────────┤
   │ SECTIONS                                                           │
   │   .text        executable machine code                             │
   │   .rodata      read-only constants (string literals, vtables)      │
   │   .data        initialized writable globals                        │
   │   .bss         zero-initialized globals (takes no file space)      │
   │   .symtab      symbol table: names ↔ addresses/section+offset      │
   │   .rela.text   relocations for .text (the "to-do list")            │
   │   .debug_*     DWARF debug info (optional)                         │
   └────────────────────────────────────────────────────────────────────┘
```

Inspect a real one:

```bash
>>> llvm-readobj --sections square.o     # list sections
>>> llvm-nm square.o                       # symbols: T=text def, D=data, U=undefined
0000000000000000 T square
                 U printf                  # ◀ undefined: needs resolving at link
>>> llvm-objdump -dr square.o              # disassemble WITH relocations annotated
   12: e8 00 00 00 00   call 0x17
        0000000000000013: R_X86_64_PLT32 printf-0x4   ◀ the relocation
```

That `00 00 00 00` in the `call` is the blank; the relocation says "patch these 4 bytes with
printf's address (PLT-relative)."

---

## 3. Symbols: the vocabulary of linking

```
   A SYMBOL is a named entity (function or data) with linkage. Two kinds matter:
     DEFINED    "I provide `square` — here it is at this offset."
     UNDEFINED  "I USE `printf` — somebody please provide it."

   Linking = matching every UNDEFINED symbol to a DEFINED one, then patching.
```

```
   square.o                    libc.a / libc.so
   ─────────                   ────────────────
   defines: square, main       defines: printf, sin, malloc, ...
   needs:   printf  ───────────────▶ found here! resolve.
```

Linkage types (from IR, chapter 00.03) map to symbol-table visibility:

```
   external   → a global symbol other objects can see/resolve against
   internal   → local symbol, not exported (C `static`)
   private    → not even in the symbol table
   weak       → can be overridden by a strong def (no conflict if duplicated)
```

This is why IR linkage matters: it controls what the linker (or JIT) can resolve.

---

## 4. What the linker actually does

```
   ┌────────────────────────── linker (ld / lld) ────────────────────────────┐
   │ 1. SECTION MERGING: concatenate all .text into one, all .data into one. │
   │ 2. SYMBOL RESOLUTION: match every undefined symbol to a definition,     │
   │    pulling in needed objects from archives (.a) as required.            │
   │ 3. LAYOUT: assign final addresses to every section/symbol.              │
   │ 4. RELOCATION: walk each relocation, compute the now-known address,     │
   │    and patch the bytes.                                                 │
   │ 5. WRITE: emit the executable (or shared library) with an entry point.  │
   └─────────────────────────────────────────────────────────────────────────┘
```

```
   BEFORE (relocatable .o)           AFTER (linked executable)
   ───────────────────────           ────────────────────────────
   call <0x00000000>  + reloc        call <0x401050>   ◀ printf's real address
   "printf is UNDEFINED"             printf defined (from libc), address known
   no fixed load address             segments laid out at known addresses
```

Static vs dynamic at this step:

```
   STATIC:  copy printf's code from libc.a INTO the exe; resolve fully now.
            → big, self-contained, no runtime resolution.
   DYNAMIC: leave a reference to libc.so; the OS's dynamic LOADER resolves
            printf at PROCESS STARTUP (via PLT/GOT indirection).
            → small exe, shared libc, but needs the .so present.
```

---

## 5. The PLT/GOT dance (dynamic linking, briefly)

For dynamically-linked calls, the address isn't known until load time, so calls go through
indirection tables:

```
   call printf
      │
      ▼
   PLT[printf]  (a stub)                  GOT[printf]  (a pointer slot)
      │  first call: jump to resolver ───▶ filled in by dynamic loader
      │  later calls: jump straight ─────▶ printf's real address in libc.so
      ▼
   printf in libc.so
```

```
   PLT = Procedure Linkage Table (code stubs)
   GOT = Global Offset Table (data: resolved addresses)
   "Lazy binding": the first call resolves & caches the address; subsequent
   calls are direct. This is why the linker emits R_X86_64_PLT32 relocations.
```

You don't implement this — but recognizing it explains "why is there a PLT32 relocation?" and
foreshadows how a JIT must also fix up cross-references.

---

## 6. The runtime startup sequence

A linked executable doesn't begin at `main`. The C runtime (crt) wraps it:

```
   OS exec() loads segments, jumps to the ENTRY POINT (_start, from crt1.o)
      │
   _start:
      │  set up the stack, fetch argc/argv/envp from the kernel
      │  call __libc_start_main(...)
      ▼
   __libc_start_main:
      │  run .init_array (global constructors, e.g. C++ static objects)
      │  call main(argc, argv, envp)        ◀── YOUR code finally runs
      ▼
   main returns an int
      │
   __libc_start_main:
      │  run .fini_array (destructors), flush stdio
      │  call exit(status) → _exit syscall
      ▼
   process terminates with that exit status
```

This is why your synthesized `main` (previous chapter) is enough: the runtime calls it for
you. You only ever wrote `main`; crt did the rest.

---

## 7. How this maps to the JIT (the payoff)

A JIT does steps 1–4 of the linker **in memory, at runtime**:

```
   AOT linker                          JIT (ORC) "linker"
   ──────────────────────────          ──────────────────────────────────
   merge sections on disk              allocate executable memory in-process
   resolve symbols across .o/.a        resolve symbols across JITDylibs +
                                       the host process's own symbols
   assign final disk/load addresses    assign addresses = the mmap'd memory
   patch relocations, write exe        patch relocations IN PLACE, mark RX,
                                       hand back a function pointer
```

```
   ┌─────────────────────────────────────────────────────────────────────────┐
   │ A JIT is "a linker that runs at runtime and targets memory instead of   │
   │ a file." Everything you learned here — symbols, relocations, fixups —   │
   │ happens in ORC's JITLink/RuntimeDyld, just live and in-process.         │
   └─────────────────────────────────────────────────────────────────────────┘
```

Hold that thought going into section 04 — it makes ORC's layers click immediately.

---

## Mental model checkpoint

1. Why can't a freshly-emitted `.o` be executed directly? What two things does it not know?
2. Define "relocation" in one sentence.
3. What is the difference between a defined and an undefined symbol, and what does linking do
   with them?
4. List the five jobs of a linker.
5. Static vs dynamic linking: who resolves `printf`, and when, in each case?
6. What are the PLT and GOT for?
7. Walk the startup path from `_start` to `main` to `exit`.
8. In one sentence, how is a JIT "a linker that runs at runtime"?

AOT complete. Next, JIT compilation → [../04-jit/01-jit-theory.md](../04-jit/01-jit-theory.md)

# Dynamic recompilation: ARMv6 → arm64

How iOS3-VM stops being 60x too slow.

This document is a **design**. Part of J2 now exists; §0 records exactly how
much, and the rest of the document is still ahead of the code.

Every factual claim is labelled:

- **CONFIRMED** — measured or read directly out of this repository at
  `65c9240`, by the commands shown. Reproduce it before trusting it. Where a
  CONFIRMED claim has since been *acted on* — the missing `-O` flags in §1.1/§1.5,
  the snapshot proposal in §11.2 — it is struck through or marked, rather than
  deleted, so the measurement that motivated the change is still readable.
- **INFERRED** — follows from something confirmed plus how ARMv8 / iOS / this
  class of emulator works. Strong, but not verified on this hardware.
- **GUESS** — plausible, unverified, and flagged so it does not quietly become
  an assumption.

The headline conclusions, up front, because two of them are not what you would
expect:

> **1. There is a measured 5–6x sitting on the floor that is not a JIT.** ~~The
> committed build compiles the core with *no optimiser at all*.~~ **Taken:** the
> top-level `CMakeLists.txt` has defaulted to `Release` since `b3940fa`, worth a
> measured **3.0x on a real kernel boot**. The software TLB — another **~2x**, and
> the JIT's memory fast path anyway — is still on the floor.
>
> **2. The JIT should start after the first SpringBoard frame renders, not
> before — but the thing that makes M5's iteration loop bearable is
> snapshot/restore, not the JIT.** A boot to SpringBoard is billions of
> instructions; no realistic interpreter speed makes that a fast edit-run loop,
> and no realistic *JIT* speed does either. Save the machine after the kernel is
> up and resume from there. **Done, and it worked** — see §11.2.
>
> **3. Do not promise realtime.** An honest, well-built block JIT on this host
> lands at **0.15–0.45x** of the guest's nominal 412 MHz (§10.3). That is
> "SpringBoard responds to your finger and animates at roughly quarter speed",
> which is the difference between a screenshot and a demo. It is not
> "indistinguishable from a real iPhone", and scheduling should not assume it.

---

## 0. Implementation status

Built so far, behind the CMake option `IOS3VM_JIT` (**OFF by default**, and the
default build and the boot path are untouched):

| | State |
|---|---|
| `core/src/jit/a64_emit.c` — AArch64 emitter | **works**, 172 byte-exact assertions in `core/tests/test_a64_emit.c`, including an exhaustive check of the bitmask-immediate encoder against an independent `DecodeBitMasks` |
| `core/src/jit/jit_translate.c` — block translator | **works** for a small ARM subset **and a measured-useful Thumb-1 subset**; 286 assertions in `core/tests/test_jit.c` on block shape and emitted words |
| `core/src/jit/jit_mem.c` — code-buffer shim | plain RWX and `MAP_JIT` + `pthread_jit_write_protect_np`; dual mapping is not implemented (§8.2 explains why it would not help on iOS anyway) |
| §3.4 dispatch, §3.5 chaining, §3.6 invalidation | **not built.** There is no code cache, so nothing calls the translator yet |
| §6.1 software TLB (J1) | **not built.** Loads and stores go through a helper that calls `arm_mmu_translate` directly |
| Thumb (J4) | **translation done**, 93.54% of retired Thumb instructions (see below) |

ARM coverage is deliberately narrow: data-processing with a rotated immediate
or an immediate-shifted register, `LDR`/`STR` word and byte with an immediate
offset and no writeback, and `B`/`BL` — all with condition codes.

Thumb coverage is broad, because §1.3 says it has to be. Everything in the
16-bit space is translated except the block transfers (`PUSH`/`POP`/`LDMIA`/
`STMIA`), the four shift-by-register ALU forms, the hi-register forms that
write PC, and the mode-changing and undefined space.

**Everything else, including every form listed as DECLINED or DEFERRED in
`jit_translate.c`, ends the block and is executed by `arm_step()`**, which is
§2's rule and is verified by a test that denies every class and checks the
translator then declines everything.

**Measured coverage** (method as §1.1: first 20,000,000 retired instructions of
the real `iPhone1,2_3.1.3_7E18` kernelcache; each retired instruction probed by
translating a one-instruction block at its PC), **CONFIRMED**:

| | before J4 | after J4 |
|---|---|---|
| share of retired instructions translated natively | **23.96%** | **88.46%** |
| …of the ARM 31.05% | 77.17% | 77.17% |
| …of the Thumb 68.95% | 0.00% | **93.54%** |

The 11.54% still handed to `arm_step()` is 7.09% ARM (LDM/STM, exclusives,
multiplies, MCR/MRC, PC-writing forms, the unconditional space) and 4.45%
Thumb, itemised in the DECLINED table at the head of the Thumb section of
`jit_translate.c`. The single largest remaining item in either instruction set
is now the block-transfer family — 5.18% ARM plus 4.38% Thumb — which §7.3
says must be a state helper wrapping the interpreter's own
`exec_block_transfer`, and which therefore needs the full-sync helper ABI of
§4.4 rather than more decoding.

Not yet demonstrated: that emitted code executes correctly. The dev box is x86.
`core/tests/test_jit_exec.c` runs the emitted code and lockstep-compares a
translated block against the interpreter, but it only does so on an arm64 host
and skips itself elsewhere; the `jit` job in `.github/workflows/core-tests.yml`
is what puts it on Apple Silicon runners. A `SKIP` on a macOS runner would mean
that coverage silently vanished, so read the log rather than the badge.

### 0.1 How much of a JIT this is: about 15%

Stated as a number because "foundation merged" is the kind of phrase that quietly
becomes "we have a JIT".

| | |
|---|---|
| Emitted-code coverage of retired instructions | **~31% ceiling** — ARM only. Thumb is 68.95% (§1.3) and is declined outright |
| Actual coverage today | far below that ceiling: only the narrow ARM subset listed above translates natively |
| Speed of a translated block **today** | **worse than interpreting it.** Mean run is 8.1 instructions (§1.3) against a ~37-instruction prologue/epilogue, and with no chaining every block pays it |
| Blocks executed in a boot | **zero.** There is no code cache and no dispatcher, so nothing calls the translator from the run loop |

Everything that turns those rows around is §3 (cache, dispatch, chaining,
invalidation), §6.1 (the software TLB), and J4 (Thumb) — which is to say, most of
this document. The merge exists so the emitter and translator stop rotting
against a core that is moving fast, and so their tests run in CI on every push.
It is not a step towards shipping the JIT sooner than §11 says.

**What it does already prove** is worth keeping separate from what it does not:
the AArch64 encodings are byte-exact against an independent decoder, the
translator declines everything it does not handle (verified by a test that denies
every class and checks it then declines the lot), and the "JIT is a layer over the
interpreter" rule of §2 holds in code rather than only in prose.

---

## 1. The measurements this design is built on

A dynarec designed from general principles will optimise the wrong things. Every
structural decision below is anchored to a number measured on this repository.

**Method.** Host: the project's Windows dev box, x86-64, MinGW GCC 15.2. Guest
workload: the real `iPhone1,2_3.1.3_7E18` kernelcache already in `firmware/`,
run by `tools/bootkernel.c`, first 20,000,000 retired instructions
(`bootkernel firmware/kernel.macho -d firmware/devicetree.bin -n 20000000`). The
instruction-mix figures come from a patched *copy* of that tool in a scratch
directory, classifying each instruction at the point it is about to retire using
the interpreter's own decode masks. Numbers are desktop numbers; the A9 will
differ, and §10.3 says by how much and why that is a guess.

### 1.1 How fast the interpreter really is

| Build | 20M instructions of real XNU | Rate |
|---|---|---|
| As committed (`build/`, **no `-O`**) | 15.88 s | **1.26 M insn/s** |
| Same sources, same tool, `-O3 -DNDEBUG` | 5.30 s | **3.77 M insn/s** |

**CONFIRMED.** Both figures include all of `bootkernel`'s per-instruction
instrumentation (a sampling profiler, STREX pre-decode, FIQ accounting, a trace
ring), so they understate the bare interpreter. Stripped of the harness, on
synthetic loops:

| Core build | ALU/branch loop | load/store loop |
|---|---|---|
| No `-O` (as committed) | 4.38 M/s | 4.20 M/s |
| `-O2` | 8.59 M/s | 8.71 M/s |
| `-O3` | 14.25 M/s | 13.72 M/s |

**CONFIRMED.** The project's own working figure of ~6.5 M insn/s for real code
sits exactly where it should between these: real code is branchier and more
Thumb-heavy than a four-instruction loop, and it is presumably from an optimised
build.

The first conclusion was uncomfortable and has since been acted on: **the
committed CMake configuration was leaving a 3x on the table.** `CMAKE_BUILD_TYPE`
was empty and the compile line carried no `-O` flag. Since `b3940fa` the
top-level `CMakeLists.txt` defaults to `Release`, so the "as committed" rows
above are now **historical** — they record what the un-optimised build cost, not
what a fresh clone does. CI builds both `Release` and a sanitizer `Debug`, which
also earns its keep as a correctness check: if `-O2` and `-O0` ever disagree,
that is a real bug worth catching.

### 1.2 Where the interpreter's time goes

Same load/store loop, core built `-O2`, varying only what the interpreter has to
do per instruction:

| Configuration | Rate | Cost of the feature |
|---|---|---|
| MMU off, no device tick | 12.09 M/s | baseline |
| MMU off, `s5l8900_tick(m,1)` per instruction | 9.51 M/s | **−21%** |
| MMU on, 1 MB sections, no tick | 7.91 M/s | −35% |
| MMU on, 4 KB small pages, no tick | 5.91 M/s | **−51%** |
| MMU on, 4 KB pages + tick per instruction | 5.32 M/s | −56% |

**CONFIRMED.** Two things fall out:

1. **The page-table walk is half the interpreter.** There is no TLB. Every
   instruction fetch calls `arm_mmu_translate`, which with a two-level mapping
   performs *two* guest memory reads through the full `bus_read` dispatch chain
   — and every data access does the same again. At the measured 22.6% memory
   instructions (§1.3) that is about **2.5 table-walk reads per retired guest
   instruction**. XNU maps the kernel with sections *and* pages; the 4 KB row is
   the honest one.
2. **The device tick costs a fifth of the interpreter.** `s5l8900_tick` runs a
   64-bit divide *and* a 64-bit modulo per guest instruction to convert retired
   instructions into timebase ticks at the 412 MHz : 6 MHz ratio
   (`core/src/soc/machine.c:229`). One tick per instruction is also 68x finer
   than the timebase can represent.

### 1.3 What the guest actually executes

First 20M instructions of the real kernel, classified at retirement
(**CONFIRMED**):

| | share of retired instructions |
|---|---|
| **Thumb** | **68.95%** |
| ARM | 31.05% |
| ARM conditional, condition failed | 5.30% |
| ARM data-processing | 17.86% |
| ARM single LDR/STR | 2.53% |
| ARM LDM/STM | 0.80% |
| ARM branch (B/BL) | 2.50% |
| ARM exclusives + SWP | 1.21% |
| ARM MCR/MRC | 0.52% |
| ARM multiply | 0.33% |
| Thumb LDR/STR (incl. SP- and PC-relative) | 14.93% |
| Thumb PUSH/POP/LDMIA/STMIA | 4.38% |
| Thumb branch/call | 11.12% |
| Thumb other (ALU, shifts, hi-reg, extends) | 38.53% |
| **taken control transfers** | **12.34%** → mean run of **8.1 instructions** |
| distinct instruction addresses executed | 37,677 |
| distinct block-entry addresses | 5,463 |
| mean executions per distinct instruction | **530.8** |

And from `bootkernel`'s own report over the same run (**CONFIRMED**):

- **4 non-RAM physical pages touched, ~180 MMIO accesses total** (UART 40,
  timer 135, VIC 4, one stray read of physical 0).
- **3 FIQ entries**, 157 instructions spent in FIQ — 0.0% of the run.
- **120,967 STREX-family instructions executed, 0 failed.**

Five design consequences, each of which changes what gets built first:

1. **Thumb is not a phase-two nicety. It is the majority of the workload, in the
   kernel, before userland even exists.** A JIT that handles only ARM covers
   31% of instructions and will be *slower* than the interpreter it replaces on
   the other 69% once dispatch overhead is added. SpringBoard is Thumb-compiled
   too, so this only gets worse.
2. **Blocks are short: ~8 instructions between taken transfers.** Per-block
   overhead is therefore the dominant design constraint. Anything costing 5
   instructions per block costs 0.6 instructions per guest instruction. This is
   what makes block *chaining* mandatory rather than an optimisation.
3. **Code is executed ~530 times on average, and the working set is tiny**
   (5,463 blocks for the whole of early kernel init). Translation cost is
   irrelevant; a slow, careful, one-pass translator is fine. Do not build a
   tracing JIT or a re-optimising tier.
4. **MMIO is statistically nonexistent** — about one access per 110,000
   instructions. The MMIO slow path needs to be *correct*, not fast. All the
   engineering belongs in the RAM fast path.
5. **Interrupts are rare** (3 in 20M). Sampling them at block boundaries instead
   of per instruction costs nothing in responsiveness.

### 1.4 What "realtime" means here, precisely

The machine model defines guest time as a function of *retired instructions*:
`S5L8900_CPU_HZ` (412 MHz) retired instructions advance the timebase by
`S5L8900_TB_HZ` (6 MHz) ticks, with the remainder carried
(`core/include/soc.h:142`, `core/src/soc/machine.c:229`). One retired
instruction is charged as one CPU cycle, which `soc.h` correctly calls
optimistic for an ARM1176.

So:

- **1.0x "realtime" by the model = 412 M guest instructions/sec.** At the
  committed build's 1.26 M/s that is **327x short**; at an optimised 6.5 M/s it
  is **63x short**. This is where the project's "60x" comes from, and it is the
  number to quote.
- **1.0x "as fast as the phone actually felt" is a smaller number.** A real
  ARM1176 at 412 MHz does not retire one instruction per cycle; ~0.6 IPC is a
  fair figure for compiled code with cache misses on that pipeline
  (**INFERRED**). Matching the *observed* speed of a 2G iPhone therefore needs
  roughly **250 M insn/s**, not 412 M.
- **Because guest time is tied to guest work, running slow does not break the
  guest.** The kernel's decrementer, `mach_absolute_time`, and every timeout
  scale together. A 4x-slow emulator is not a stuttering iPhone with watchdog
  panics; it is an iPhone whose animations play at quarter speed and whose
  touches are handled a quarter as fast. That is the property that makes a
  sub-realtime JIT genuinely usable, and it is worth stating out loud before
  anyone proposes chasing 1.0x.

### 1.5 Three cheap wins that are not a JIT

| Win | Measured / expected | Risk | Is it wasted if the JIT happens? |
|---|---|---|---|
| ~~Build with `-O2`/`-O3`~~ **TAKEN** (`b3940fa`) | **3.0x, measured** (§1.1) | none | no |
| Software TLB in front of `arm_mmu_translate` | **~1.7–2.0x** (from §1.2's 5.91 → ~12 M/s ceiling) | *real* — see §6.1 | **no**: it *is* the JIT's memory fast path |
| Tick devices once per batch instead of per instruction | **~1.2x** (§1.2) | low | **no**: it is required for the JIT to be differentially testable (§9.3) |

Compounded, that is roughly **5–6x on the interpreter alone**, taking the
committed build from ~1.3 M insn/s to ~7–8 M insn/s of real kernel code on this
desktop, with no new instruction semantics written and therefore essentially no
new correctness risk (the TLB excepted — §6.1 explains exactly where its danger
is).

It also moves the JIT's job from "make up 327x" to "make up 60x", and every one
of the three is a prerequisite or a co-requisite of the JIT. **None of it is
throwaway.**

---

## 2. The structural decision everything else depends on

> **The JIT is a layer over the interpreter, never a replacement for it. Any
> instruction the translator does not handle is emitted as a call to
> `arm_step()`.**

This single rule buys:

- **Incremental delivery.** The JIT is useful at 20% instruction coverage. There
  is never a "big bang" where the JIT must be complete before anything boots.
- **A guaranteed correctness floor.** An instruction the JIT does not implement
  cannot be implemented *wrongly*, because it is executed by the same C function
  the 142-test suite covers and the boot has been debugged against.
- **A debugging escape hatch.** `IOS3VM_JIT_DENY=<encoding-class>` forces any
  class back to the interpreter, so a divergence can be bisected by *feature*,
  not just by instruction address.
- **Coverage of the ~5% the interpreter itself still traps** (media/DSP,
  LDRD/STRD, VFP arithmetic, SETEND BE). The JIT inherits `ARM_UNDEFINED`
  handling for free and never has to reimplement it.

The interpreter therefore stays the specification. `arm_interp.c` must not be
modified for the JIT's convenience; the only interpreter-side changes this
design asks for are behaviour-preserving (the TLB, tick batching, and a
`arm_run(cpu, budget)` entry point beside `arm_step`).

---

## 3. Translation unit, block keys, and the code cache

### 3.1 Basic blocks, not traces

Measured mean run length is 8.1 instructions (§1.3) and mean reuse is 530x. That
combination says: translate **basic blocks**, chain them directly to each other,
and never build traces or superblocks.

- Traces pay off when block dispatch is expensive relative to block length.
  With direct chaining, dispatch between two known blocks is *one branch
  instruction*, so there is nothing left for a trace to amortise.
- Traces multiply the invalidation problem: one written code page invalidates
  every trace that inlined it.
- Traces need profiling, tiering, and side-exit bookkeeping — a large amount of
  machinery for a workload whose hot set is 5,463 blocks.

Cap block length at **64 guest instructions** and end at a 4 KB page boundary
(§3.2). The cap bounds interrupt latency (§7.2) and bounds the damage from a
single invalidation.

### 3.2 What ends a block

A block ends *after* an instruction that:

1. writes r15 by any route (B/BL/BX/BLX, `MOV pc,..`, `LDR pc,..`, `LDM {..,pc}`,
   Thumb POP{pc}, the hi-register ADD/MOV forms);
2. can change CPSR mode or the T bit (MSR, CPS, SRS/RFE, `MOVS pc,lr`,
   `LDM {..,pc}^`, SWI, anything the interpreter routes through
   `take_exception`);
3. writes CP15 state that changes translation (SCTLR, TTBR0/1, TTBCR, DACR,
   CONTEXTIDR, FCSE, or any c8 TLB-maintenance op) — because the meaning of the
   *next* fetch may have changed;
4. is not implemented by the translator (falls back to `arm_step`, §2) — the
   block may continue after it only if that instruction cannot have written PC
   or CPSR, which is not knowable for the general fallback, so: end the block;
5. would cross a 4 KB guest page boundary — the next instruction lives in a
   different page with a different mapping and different invalidation identity;
6. is the 64th instruction of the block.

Note what does *not* end a block: a conditional instruction whose condition
fails (5.3% of instructions — these are emitted as a short forward branch inside
the block), a load or store (aborts are handled by exits, §7.3), or an
LDREX/STREX pair.

### 3.3 The block key

This is the part most likely to be got wrong, because the obvious key — the PC —
is wrong three different ways.

**The key is a struct, not an address:**

```c
typedef struct {
    uint32_t va;        /* virtual address of the first instruction        */
    uint32_t pa;        /* physical address it translated to               */
    uint16_t flags;     /* THUMB | PRIVILEGED                              */
    uint16_t ctx;       /* address-space tag (see below)                   */
} jit_key_t;
```

- **`va` must be in the key.** Emitted code bakes in PC-derived constants: every
  branch target, every `pc+8` operand (§7.5), every Thumb PC-relative literal
  address. The same bytes at a different VA are a different block.
- **`pa` must be in the key.** The same VA means different code in different
  address spaces. XNU switches TTBR0 on every context switch; two processes both
  have code at `0x2000`. Keying on VA alone would execute one process's code in
  another. `pa` also gives invalidation a physical identity (§3.6) and makes
  aliasing work: the kernel maps physical pages both in its virtual range and in
  the physical aperture, and a block translated through one alias is
  *semantically* usable through the other only if the VA matches too — which the
  `va` field enforces.
- **`flags.THUMB` must be in the key.** The same address decoded as ARM and as
  Thumb is two entirely different programs. The interpreter switches on
  `ARM_CPSR_T` at `arm_interp.c:1273`; the JIT switches at translate time.
- **`flags.PRIVILEGED` must be in the key**, because the emitted memory fast
  path selects the privileged or unprivileged TLB array at translate time
  (§6.1). (An alternative is to select at runtime from a register, at the cost
  of one extra instruction per access; keying is cheaper and blocks are not
  shared between kernel and user code in practice.)
- **`ctx` is a monotonic address-space tag**, allocated by the runtime whenever
  it sees a new `(TTBR0[31:14], CONTEXTIDR.ASID, FCSE_PID)` triple, in a small
  direct-mapped table. It is *not* strictly required — `pa` already disambiguates
  — but it makes the fast dispatch path (§3.4) correct without a translation on
  every block entry, and it is what the ARMv7 port will need anyway (§12).

Deliberately **not** in the key: DACR, AP bits, SCTLR.M. Permissions are checked
by the TLB at *runtime*, not baked into emitted code, so a permission change
needs a TLB flush (§6.1) but not a code flush. Endianness is not in the key
because the interpreter refuses `SETEND BE` (`arm_interp.c:1420`); if that ever
changes, it joins `flags`.

### 3.4 Dispatch

Two tiers, because translating a VA to a PA on every block entry would cost more
than the block.

**Tier 1 — the virtual jump cache.** A direct-mapped array of 4096 entries
indexed by `hash(va, flags)`, each `{ va, flags, ctx, code_ptr }`. Hit = ~5 host
instructions. Entries are validated against the current `ctx`, so a context
switch invalidates the whole cache *implicitly*, without touching memory.

**Tier 2 — the physical hash table.** On a tier-1 miss: translate `va` through
the instruction TLB (filling it via `arm_mmu_translate` on a miss, which may
raise a prefetch abort exactly as `arm_step` does at `arm_interp.c:1261`), then
look up `(pa, va, flags)` in an open-addressed hash table of all live blocks. On
a miss, translate a new block. Then install the result in tier 1.

**Tier 0 — chaining** (§3.5) bypasses both for the common case.

### 3.5 Chaining and the indirect-branch cache

**Direct branches.** When a block ends in a branch to a translate-time-known VA,
emit the exit as a `B` to a patchable stub. The first time it is taken, the stub
calls the dispatcher, which — if the target block exists and is in the same
`ctx` — rewrites the `B` to jump straight at the target's entry, then performs
the cache maintenance of §8.4 for that one instruction. Every subsequent
execution is a single branch. For a conditional branch, both edges are patched
independently.

**Indirect branches** (BX, `LDM {..,pc}`, POP{pc}, `MOV pc,lr` — a large share
of the 12.34% taken transfers, since every function return is one) cannot be
patched. Emit an inline probe of a small direct-mapped **indirect-branch target
cache**: index by `(target_va >> 1) & 255`, compare the stored VA and `ctx`,
branch to the stored code pointer on a hit. ~6 instructions, versus ~20+ for a
dispatcher round trip. **INFERRED** that this matters; measure it at J5 before
keeping it.

**Every chain must be recorded**, so that invalidating a block can unlink its
predecessors (§3.6). Each block carries a list of incoming patch sites.

### 3.6 Invalidation

Four events invalidate translated code. Only the first is subtle.

**(a) The guest writes to memory containing translated code.** This is not a
corner case: XNU decompresses and relocates kexts into RAM, and the pager will
later page executable code in. The mechanism:

- Maintain a bitmap with one bit per 4 KB of guest RAM (128 MB → 4 KB of
  bitmap). A bit is set when a block is translated out of that physical page.
- **Poison the write-TLB entry for any code page.** A store to a code page then
  cannot hit the inline fast path (§6.2) — it always calls the slow-path helper,
  which invalidates every block registered on that page, unlinks their chains,
  clears the bit if no blocks remain, and *then* performs the store.
- Cost on non-code pages: **zero**. This is the whole reason the poisoning
  approach is preferred to a check-on-every-store.
- Blocks are registered on the physical page of *every* instruction they cover,
  not just their first — which is why blocks may not span pages (§3.2 rule 5).

**(b) The guest executes a cache-maintenance operation.** CP15 c7 is currently a
documented no-op (`arm_interp.c:266`). Under a JIT it becomes a free safety net:
treat `ICIALLU` as "flush the code cache" and `ICIMVAU` as "invalidate this
page". Mechanism (a) should already have caught everything; honouring c7 costs
nothing and covers any path where it did not (a host-side loader writing guest
RAM directly, for instance — `s5l8900_load` must call the same invalidation
entry point).

**(c) TTBR0 / CONTEXTIDR / FCSE change (a context switch).** This invalidates
**nothing**. Blocks stay valid because they are keyed physically; only the
`ctx` tag changes, which implicitly empties the tier-1 jump cache and the
indirect-branch cache. This matters enormously: XNU context-switches constantly,
and an implementation that flushed the code cache on every switch would spend
all its time retranslating. It is the single strongest argument for including
`pa` in the key.

**(d) The code cache fills.** Flush everything and reset the arena (§3.7).

### 3.7 Code cache sizing and flush policy

Measured: 5,463 distinct block entries in the first 20M instructions of kernel
init (§1.3). Extrapolating to a full boot with kexts, launchd, and SpringBoard:
**50k–300k blocks (GUESS)**. At ~8 guest instructions and ~4 host instructions
per guest instruction plus a header, ~200 bytes/block → **10–60 MB**.

Allocate a **64 MB** RWX arena, bump-allocate into it, and on exhaustion **flush
everything**: drop all blocks, clear all bitmaps, reset the arena. No LRU, no
per-block freeing — with 530x mean reuse the retranslation cost after a flush is
recovered in milliseconds, and the bookkeeping saved is substantial. The app
already carries `com.apple.developer.kernel.increased-memory-limit`
(CONFIRMED, `app/iOS3VM.entitlements` per `ARCHITECTURE.md`), which is what
makes 64 MB on top of a 128 MB guest RAM acceptable on a 2 GB device.

**Flush is also a test tool**: a "torture mode" that flushes every N blocks
exercises translation and invalidation paths that would otherwise run once
(§9.6).

---

## 4. Register allocation

### 4.1 The fixed mapping

The mapping must be **fixed across all blocks** — chaining means one block
branches into another with no glue code, so both must agree on where the guest
state lives.

| arm64 | holds | why |
|---|---|---|
| `x0`–`x7` | helper arguments / scratch | AAPCS64 |
| `x8`–`x15` | translation scratch | caller-saved, free inside a block |
| `x16`, `x17` | scratch, never live across a call | IP0/IP1, may be clobbered by linker veneers |
| **`x18`** | **untouched** | **reserved by Apple's platform ABI — using it is not merely discouraged, it is unsafe** (INFERRED, but universally documented) |
| `x19`–`x26` | **guest r0–r7** | see below |
| `x27` | **guest r13 (SP)** | see below |
| `x28` | **`arm_cpu_t *`** | every spilled register, CPSR, CP15 and the TLB are reached from here |
| `x29` | frame pointer, maintained | keeps crash reports and the debugger usable inside JIT code |
| `x30` | link register | helper calls |
| `NZCV` | **guest N, Z, C, V** | §5 |

Guest **r8–r12, r14, r15** live in the `arm_cpu_t` and are loaded/stored on
demand, with per-block caching in `x8`–`x15` (allocated by the translator within
a block, written back before any block exit or helper call).

**Why exactly this set.** It is chosen from the measured mix, not from
aesthetics. 68.95% of retired instructions are Thumb, and 16-bit Thumb can only
address **r0–r7, SP and LR** in almost every encoding. Pinning r0–r7 and SP
covers essentially all Thumb register traffic and the majority of ARM traffic
(r0–r3 arguments, r4–r7 callee-saved locals). Extending the pinned set upward
would displace the CPU pointer, which is needed by every spill, every helper
call, and every TLB access.

The obvious complaint is that **r14 (LR) is not pinned**, and LR is written by
every BL and read by every return. The answer is that both of those *end a
block* (§3.2), so LR traffic is one store or one load at a block boundary, never
in the middle of a hot straight line.

### 4.2 Why r8–r12 in memory makes banking free

This is a happy accident worth spelling out, because it is the reason not to
"improve" the mapping later.

ARM banks r13/r14 per privileged mode and additionally banks **r8–r12 for FIQ
only** (`arm_set_mode`, `arm_interp.c:83`). With r8–r12 memory-resident, the FIQ
bank swap is *entirely* a memory-to-memory operation inside `arm_set_mode`, and
that function works under the JIT completely unmodified. Only r13 (pinned in
`x27`) and r14 (memory) need any JIT-side handling on a mode change, and r14 is
already in memory.

So the mode-switch protocol is: **spill `x19`–`x27` and NZCV to the
`arm_cpu_t`, call the interpreter's own `arm_set_mode`, reload.** Nine stores,
one call, nine loads — and provably identical banking behaviour, because it *is*
the interpreter's banking code.

### 4.3 Mode switches

Every instruction that can change mode ends a block (§3.2 rule 2) and is
executed by a helper that wraps the interpreter's own path. There is no
JIT-native implementation of MSR, CPS, SRS, RFE, SWI, `MOVS pc,lr`, or
`LDM {..,pc}^`. Measured cost of that decision: MCR/MRC is 0.52% of instructions
and mode changes are rarer still. Measured benefit: the exception-return
alignment bug family (§7.4) cannot recur in the JIT, because the JIT does not
contain a second copy of that logic.

### 4.4 The helper-call ABI

Two kinds of helper.

**Leaf helpers** (TLB miss fill, MMIO read/write, exclusive monitor ops) touch
only their arguments and the CPU struct's non-register fields. They are C
functions; the pinned guest registers `x19`–`x27` are callee-saved so the C
compiler preserves them automatically. The emitted call must only:

```
    mrs   x9, nzcv          // guest flags live in host flags
    stp   x9,  x30, [sp, #-16]!
    bl    helper
    ldp   x9,  x30, [sp], #16
    msr   nzcv, x9
```

**State helpers** (LDM/STM, anything mode-changing, the `arm_step` fallback)
need to see and modify the whole register file. They are preceded by a **full
sync**: store `x19`–`x27` into `cpu->r[0..7]` and `cpu->r[13]`, store NZCV into
`cpu->cpsr`, store the guest PC of the current instruction into `cpu->r[15]`;
and followed by a **full reload**. That is 5 STP + 5 LDP + 2 flag moves ≈ 12
instructions.

Frequency of full syncs, from §1.3: LDM/STM (5.18%) + exclusives (1.21%) +
MCR/MRC (0.52%) + block-ending mode changes ≈ **7%** of instructions, so ~0.9
host instructions per guest instruction on average. That is the price of never
writing a second implementation of LDM's base-restored abort model, and it is
worth paying (§7.3).

---

## 5. Flags

### 5.1 What maps directly

A32's NZCV and A64's NZCV are the same four bits with the same meaning, in the
same PSTATE positions, and A64 keeps ARM's carry convention for subtraction
(C set = no borrow). So:

| A32 | A64 | exact? |
|---|---|---|
| `ADDS`, `ADC`/`ADCS`, `CMN` | `ADDS`, `ADCS`, `CMN` (32-bit `w` forms) | **yes** |
| `SUBS`, `SBCS`, `RSBS`, `CMP` | `SUBS`, `SBCS`, `CMP` (RSB via operand swap) | **yes** |
| condition codes EQ..LE | identical encodings and meanings | **yes** |
| `MOVS`/`MVNS` with no shift | `ORR`+`TST`-style N/Z, C untouched | needs care, §5.2 |

The A32 condition field maps 1:1 onto A64 `B.cond` / `CSEL`, so a
condition-failed instruction (5.30% of retired instructions) is emitted as a
`B.<inverse>` over its body, or as a `CSEL` for a single-register result.

Guest NZCV live in the host NZCV between instructions **and across chained
blocks**. The rest of CPSR (I, F, T, mode) is memory-only. The Q (saturation)
bit does not exist in this design because the interpreter implements no
saturating instruction.

### 5.2 What does NOT map, and is exactly the trap the brief warns about

Four A64 facts make the naive mapping silently wrong. Each produces a bug that
is invisible in ordinary code and fatal in a lock or a `memcmp`.

**(1) A64 logical operations destroy C and V.** `ANDS`/`BICS` in A64 set N and Z
from the result and set **C = 0, V = 0**. A32's `ANDS`/`ORRS`/`EORS`/`BICS`/
`MOVS`/`MVNS` set N and Z, set **C from the barrel-shifter carry-out**, and
**preserve V** (`alu_logic_flags`, `arm_interp.c:427`). A direct mapping is
therefore wrong in two flags at once.

Emit instead: compute the value and its shifter carry-out separately, then

```
    ands  wz, wRes, wRes        // N, Z from the result; C, V now 0 (wrong)
    mrs   x9,  nzcv
    bfi   x9,  xCarry, #29, #1  // insert the shifter carry-out into C
    bfi   x9,  xSavedV, #28, #1 // restore the preserved V
    msr   nzcv, x9
```

When the shift is a constant and the carry-out is a constant (`LSL #0` — carry
unaffected), the translator folds it: for the overwhelmingly common
`ANDS/ORRS/EORS` with `LSL #0`, only V and C need preserving, which is a single
`mrs`/`bfi`/`msr` trio, or nothing at all if the block later overwrites all four
flags (§5.3).

**(2) A64 has no register-shifted operand, and its shifts are modulo 32.**
A32's `LSL Rm, Rs` with `Rs = 32` yields 0 with carry-out `Rm[0]`; `Rs > 32`
yields 0 with carry-out 0; `Rs = 0` leaves the value and the carry *both*
untouched (`barrel_shift`, `arm_interp.c:345`). A64's `LSLV Wd, Wn, Wm` uses
`Wm mod 32`, so a shift by 32 is a shift by 0 — the exact opposite answer. Every
register-specified shift must be emitted as an explicit sequence:

```
    and   w9,  wAmt, #0xff       // A32 uses Rs[7:0]
    lsl   w10, wVal, w9          // wrong for w9 >= 32, fixed next
    cmp   w9,  #32
    csel  w10, wzr, w10, hs      // >= 32 -> 0
    ...carry-out selected the same way, three cases...
```

Register-specified shifts are rare in compiled code; emitting a helper call for
them is acceptable and *safer*. **Recommendation: call a helper that is
literally `barrel_shift`,** and revisit only if profiling says otherwise.

**(3) A64 has no RRX.** `RRX` is `ROR #0` in the A32 encoding and rotates
through the carry: result `(C << 31) | (val >> 1)`, new carry `val & 1`. Emit:

```
    mrs   x9,  nzcv
    ubfx  w10, w9, #29, #1       // old C
    orr   w11, w10, lsl #31 ...  // build result
```

**(4) A32's immediate rotate sets C.** An 8-bit immediate rotated by a non-zero
amount sets C from bit 31 of the *rotated* value; rotate 0 leaves C alone
(`dp_operand2`, `arm_interp.c:387`). This is a translate-time constant — fold it
into the emitted flag update. Getting it wrong is invisible until a `MOVS
r0,#0x80000000` feeds a later `ADC`.

### 5.3 Where lazy flag evaluation is safe, and where it is not

**Do not implement deferred/lazy flags** (storing operands and computing NZCV on
demand). The interpreter is eager and exact; a lazy scheme has to reproduce it
at every observation point, and the observation points include every exception
entry (SPSR captures CPSR) and every `MRS`. The bug class it creates is exactly
the one the brief is afraid of.

**Do implement dead-flag elimination**, which is strictly local and provable.
Within one block, an instruction's flag computation may be skipped **iff, on
every path from it to any of the following, all four flags are redefined
first**:

1. a block exit (the flags are live out — they are architectural state);
2. any instruction that reads flags (a conditional instruction, ADC/SBC/RSC,
   RRX, `MRS`);
3. **any instruction that can fault** — a load, a store, or an `arm_step`
   fallback — because a data abort captures CPSR into SPSR_abt
   (`take_exception`, `arm_interp.c:132`), which makes the flags observable at
   that point;
4. any helper call (helpers may read `cpu->cpsr`).

Rule 3 is the one that is easy to forget and impossible to debug afterwards.
With it, elimination still fires on the common `CMP` / `MOVS` / `ANDS` chains in
straight-line ALU code, which is where §1.3 says a third of the time goes.

Expected value: **1.1–1.25x (GUESS)**. Schedule it at J5, not before, and gate
it behind a flag so the differential harness can run with it on and off.

### 5.4 Interpreter-defined behaviour the JIT must copy bit-for-bit

The oracle is the interpreter, not the ARM ARM. Where the interpreter picks one
of several architecturally-permitted (or simply wrong) behaviours, **the JIT
must reproduce the interpreter's choice**, and the divergence from real hardware
must be fixed in both at once, later, deliberately.

| Behaviour | Interpreter | Real ARM1176 | Where |
|---|---|---|---|
| `STR r15, [..]` stores | `pc + 8` | `pc + 12` | `arm_interp.c:629` (comment acknowledges it) |
| `Rm == 15` with a register-specified shift reads | `pc + 8` | `pc + 12` | `arm_interp.c:400` |
| C flag after `MUL`/`MLA` | unchanged | unpredictable | `arm_interp.c:848` |
| C, V after `UMULL`/`SMLAL` | unchanged | unpredictable | `arm_interp.c:896` |
| Unaligned `LDR` | reads straddling bytes (memcpy) | rotates within the word | `machine.c:95` (§6.5) |
| CP15 unmodelled registers | read 0, writes ignored | vary | `arm_interp.c:249` |

This table is a **maintenance obligation**: whenever the interpreter's choice
changes, the JIT's must change in the same commit, and the differential harness
is what proves it did.

---

## 6. Memory

Every guest access currently costs `arm_mmu_translate` (up to two full guest
reads through `bus_read`) plus a `bus_read` dispatch chain plus a `memcpy`.
Measured cost: half the interpreter (§1.2).

### 6.1 The software TLB

```c
typedef struct {          /* 16 bytes, one cache line holds four */
    uint64_t tag;         /* VA >> 12, or TLB_EMPTY (~0) */
    uint64_t addend;      /* host_addr = addend + VA */
} arm_tlb_entry_t;

#define ARM_TLB_BITS 8    /* 256 entries, direct-mapped on VA[19:12] */
```

Six arrays: {read, write, execute} × {privileged, unprivileged}. 6 × 256 × 16 B
= **24 KB**, which fits in the A9's L1D alongside working data (**INFERRED**).
Split by access type so the emitted fast path needs *no* permission check: an
entry's presence in the write-privileged array *is* the proof that a privileged
write to that page is permitted.

Fills go through `arm_mmu_translate` — unchanged, still the single source of
truth — and store `addend = host_page_ptr - (va & ~0xfff)`. Pages that are not
guest RAM (MMIO, NOR, stubs, unmapped) are **never** installed, so they always
take the slow path (§6.3).

**The TLB is the riskiest item in this whole document, and here is exactly
why.** Today, CP15 c8 TLB maintenance is a no-op and that is *correct*, because
there is no TLB to be stale (`arm_interp.c:267`). The moment a TLB exists, those
no-ops become load-bearing. The invalidation rules:

- **Any** c8 operation → flush all six arrays (a 24 KB `memset` of tags). Do not
  attempt per-VA invalidation; the cost of being wrong dwarfs the cost of being
  conservative, and c8 ops are rare.
- Any write to TTBR0/TTBR1/TTBCR/DACR/CONTEXTIDR/FCSE/SCTLR → flush, and
  allocate/lookup a new `ctx` tag (§3.3).
- A guest store into a page that is *itself* a page table would leave a stale
  entry. This is safe **only** because of an architectural rule worth writing
  down: ARMv6 does not cache faulting translations, so an invalid→valid
  transition needs no maintenance and our TLB (which only caches successful
  translations) cannot be stale for it; every *other* transition —
  valid→invalid, permission narrowing, remapping — architecturally *requires*
  the guest to issue TLB maintenance, which we honour above. **INFERRED, and it
  is the assumption the whole design rests on.** If XNU 1357 turns out to skip
  maintenance somewhere, the symptom is a stale mapping and the mitigation is
  §9.5's TLB-vs-no-TLB differential run, which would catch it deterministically.

### 6.2 The emitted fast path

A 32-bit load, guest address already computed in `w9`, destination guest r0
(`x19`), privileged block:

```
    lsr   x10, x9,  #12               // page number == tag
    and   x11, x10, #255              // direct-mapped index
    add   x11, x28, x11, lsl #4       // &cpu->tlb_read_priv[idx]  (x28 = cpu)
    ldp   x12, x13, [x11, #TLB_RD_P]  // tag, addend
    cmp   x12, x10
    b.ne  Lslow                       // ~never taken
    ldr   w19, [x13, x9]              // host_addr = addend + VA
```

**7 instructions, one predictable branch, two dependent loads.** On an
out-of-order A9 with the TLB array in L1, **~4–6 cycles (INFERRED)**.

The current path, for comparison (CONFIRMED by reading the code): one call to
`arm_mmu_translate`, which for a two-level mapping performs two `bus_read`
calls, each of which runs a chain of range comparisons before a `memcpy`; then
another `bus_read` for the access itself. Well over a hundred host instructions.
**This one sequence is where the interpreter's missing 2x and a large share of
the JIT's win both come from.**

Stores are identical with the write array. Byte and halfword accesses use
`ldrb`/`ldrh`/`strb`/`strh`. The `add x11, x28, ...` can be hoisted out of a
block with several accesses.

### 6.3 Misses, MMIO, and the slow path

`Lslow` is a leaf helper (§4.4): it calls `arm_mmu_translate`; on a fault it
latches the abort exactly as `note_abort` does and returns a "took an abort"
signal; on success, if the physical address is guest RAM it installs a TLB entry
and retries; otherwise it performs the access through `m->bus` — the same
`bus_read`/`bus_write` the interpreter uses, so UART, VIC, timer, power, NOR and
stub windows all behave identically with zero JIT-side code.

Measured MMIO frequency: **~180 accesses per 20,000,000 instructions** (§1.3).
Even at 200 host instructions each, that is 0.002% of runtime. **Spend no effort
optimising MMIO.** This will shift once the framebuffer and LCD exist, but the
framebuffer lives in DRAM (per the M4 work), so it stays on the fast path.

### 6.4 Stores to code pages

Covered in §3.6(a): code pages get **no** write-TLB entry, so their stores fall
into `Lslow`, which invalidates before storing. Non-code pages pay nothing.

### 6.5 Alignment, and an existing divergence

The bus reads via `memcpy` (`machine.c:95`), so an unaligned word load currently
returns the straddling bytes. Real ARMv6 with `SCTLR.U == 0` *rotates* the
addressed word instead. A64 `ldur`/`ldr` on Normal memory permits unaligned
access and yields the straddling bytes — i.e. **the JIT's natural behaviour
matches the interpreter's current behaviour**, and both differ from hardware in
the same way. That is the correct outcome for lockstep testing, and it belongs
in §5.4's table so that if the interpreter is ever fixed, the JIT is fixed with
it.

---

## 7. The hard cases

### 7.1 LDREX / STREX and the exclusive monitor

Measured: 1.21% of retired instructions are exclusives or SWP, with
**120,967 STREX executed and 0 failed** in 20M instructions (CONFIRMED). Every
XNU spinlock and atomic goes through them.

**Plan: helper calls for the entire family**, wrapping the interpreter's own
logic. The monitor is two fields (`excl_valid`, `excl_addr`) and the rules are
subtle in ways the codebase has already been bitten by:

- `take_exception` clears the monitor, and the comment at `arm_interp.c:141`
  records that omitting this produced two owners of one spinlock once interrupts
  started firing.
- `CLREX` must be decoded *before* `PLD` because its encoding satisfies the PLD
  pattern (`arm_interp.c:1315`) — an ordering bug that made a STREX that must
  fail succeed.

At 1.21% frequency, a ~25-instruction helper costs ~0.3 host instructions per
guest instruction. Reimplementing this inline to save that would be a
spectacularly bad trade. If profiling ever demands it, inline only the *load*
half (`LDREX` = load + two stores) and keep `STREX` in a helper.

The monitor must also be cleared wherever the interpreter clears it and nowhere
else — which is guaranteed by using the interpreter's code.

### 7.2 Taking an IRQ/FIQ at a block boundary

The interpreter samples `fiq_line` then `irq_line` at the top of *every*
`arm_step` (`arm_interp.c:1244`). A JIT that did that would spend more on
sampling than on executing.

**Plan:**

- Interrupt lines are only *changed* by `s5l8900_tick`, which the JIT calls at
  **sync points** — block boundaries where the accumulated instruction budget
  has expired. Between sync points, the lines physically cannot change.
  Therefore sampling at sync points is not an approximation of the interpreter;
  with the batched-tick interpreter of §1.5 it is **exactly equivalent**, which
  is what makes lockstep testing possible (§9.3).
- The budget is 64 instructions (matching the block cap). The timebase advances
  once per ~68 CPU cycles (412 MHz : 6 MHz), so a 64-instruction sync interval
  is **finer than the guest's own clock resolution** — the guest cannot observe
  the batching through `mach_absolute_time`.
- The budget is checked at block *entry* for chained blocks only where a cycle
  is possible: back-edges (target VA ≤ current VA) and indirect branches.
  Forward chains run free. This keeps the check off the straight-line path while
  bounding latency, since any unbounded execution must contain a cycle.
- On a pending, unmasked interrupt the dispatcher calls the interpreter's
  `take_exception` path — again, no second implementation.

Measured interrupt frequency in kernel init: **3 FIQs in 20M instructions**. A
64-instruction latency is not observable.

### 7.3 Data aborts mid-instruction and the base-restored model

The interpreter is careful here and the JIT must not be less so:

- `note_abort` latches the *first* fault only (`arm_interp.c:39`).
- Single loads do not write the destination if an abort is pending
  (`arm_interp.c:622`), and writeback is skipped.
- LDM/STM buffer every loaded word in `loaded[16]` and commit only after **all**
  accesses have succeeded (`arm_interp.c:724`), so Rn and the destination
  registers are untouched and the handler can map the page and re-execute.

**Plan:**

1. **LDM/STM/PUSH/POP are always state helpers** — the interpreter's
   `exec_block_transfer` / Thumb PUSH-POP path, called after a full sync. That
   is 5.18% of instructions at ~12 instructions of sync overhead: ~0.6 host
   instructions per guest instruction, in exchange for the base-restored abort
   model being *provably* identical. Later (J5) an inlined fast path may
   pre-probe the first and last address through the TLB and, on a hit for both,
   run an unchecked inline sequence — but only after the differential harness is
   trusted.
2. **Single accesses** are emitted so that no architecturally visible register
   is written until the access has succeeded: the value lands in a scratch
   register, and the destination and any writeback are updated afterwards. The
   `Lslow` helper returns an abort indication; the emitted code branches to a
   **block exit stub** that syncs state and enters the interpreter's data-abort
   path.
3. **`cpu->r[15]` must be correct at every faulting instruction**, because
   LR_abt is `pc + 8` (`arm_interp.c:1588`) and DFAR/DFSR are read by the
   handler. The translator therefore materialises the current instruction's
   guest PC into `cpu->r[15]` before any instruction that can fault. That is one
   `movz`/`movk` + `str` — measurably cheap and non-negotiable. (An optimisation
   for J5: store PC once per *group* of instructions between faultable ones and
   reconstruct the exact PC from a side table indexed by the host return
   address, as QEMU does. Do not do this before the harness is trusted.)

### 7.4 Exception-return alignment

There is documented bug history here and it cost a real debugging session: the
resume address must be aligned for the state the **SPSR restores**, not the
state you are currently in (`arm_interp.c:564`, and again for RFE at
`arm_interp.c:1402`). Forcing word alignment when returning into Thumb rewinds
the guest by two bytes and re-executes an instruction — the failure mode was a
zone free unlocking a mutex at address 1.

**Plan: the JIT contains no exception-return logic whatsoever.** `MOVS pc,lr`
and the whole data-processing-writes-PC-with-S family, `LDM {..,pc}^`, RFE, and
SWI all end a block and are executed by the interpreter through a state helper
(§4.3). The alignment rule then exists in exactly one place in the codebase,
forever.

`bootkernel` already reports "exception returns into Thumb" with an invariant
check that the resume address equals `lr & ~1` (CONFIRMED, its
`=== EXCEPTION RETURNS INTO THUMB ===` section). That counter must stay at zero
mismatches under the JIT, and it is a free JIT regression test.

### 7.5 The PC as an operand

`reg_read` returns `pc + 8` for r15 in ARM state (`arm_interp.c:333`), and Thumb
reads `pc + 4`, word-aligned for PC-relative loads (`arm_interp.c:918`, `:1003`).

Under a JIT this is **free**: at translate time the PC of each instruction is a
constant, so `pc+8` / `pc+4` becomes an immediate materialised with `movz`/`movk`
(two instructions, often folded into an `add`). No guest register is allocated
for r15 at all; `cpu->r[15]` is written only at faultable instructions (§7.3)
and at block exits.

Two subtleties, both already in §5.4's table: the interpreter reads `pc+8` (not
the architectural `pc+12`) for `Rm == 15` under a register-specified shift and
for a stored r15. **The JIT must emit `pc+8` in both cases** to stay in lockstep,
with a comment pointing at this document.

### 7.6 Instruction → timebase tick accounting

Today: `s5l8900_run` calls `s5l8900_tick(m, 1)` per instruction, and each call
performs a 64-bit divide and modulo (`machine.c:236`). Measured cost: **21% of
interpreter throughput** (§1.2).

**Plan, in three parts:**

1. **Batch.** Each block adds its retired-instruction count to a budget in the
   CPU struct at its epilogue (`ldr`/`add`/`str`, hoisted so chained forward
   blocks accumulate without checking). At a sync point, call
   `s5l8900_tick(m, budget)` once. Because `tb_accum` carries the remainder, the
   ratio stays exact over time regardless of batch size — the existing code is
   already written for this.
2. **Replace the divide** with a reciprocal multiply-shift for the fixed
   412 MHz : 6 MHz ratio, falling back to the divide when `cpu_hz`/`tb_hz` are
   set to anything else (the on-device self-test sets both to 1 —
   `EmulatorViewController.m:124`, CONFIRMED — so the fallback is not
   hypothetical).
3. **`cpu->cycles` must match the interpreter exactly**, including the
   increments on exception entry (`arm_interp.c:1246`, `:1253`, `:1264`), since
   the differential harness compares it.

**Do the batching in the interpreter first (J1).** It is a 20% interpreter win
*and* it is what makes the two engines comparable (§9.3).

### 7.7 Self-modifying code, kexts, and paging

Covered by §3.6. The point worth repeating: the dangerous case in this project is
not classic SMC, it is **bulk code writes** — XNU relocating and linking kexts,
and later the pager faulting executable pages in. The poisoned-write-TLB design
handles bulk writes at the cost of one helper call per store *to a page that
currently holds translated code*, which during a kext load is exactly the
behaviour wanted: the first store to a code page throws its blocks away, and the
remaining stores to that page are then ordinary fast-path stores because the page
no longer holds code.

---

## 8. The host: A9, iOS 15, jailbroken

### 8.1 What is confirmed

- The app **already probes both preconditions at launch** and prints them:
  `csops(CS_OPS_STATUS)` for `CS_DEBUGGED`, and a plain
  `mmap(PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON)`
  (`app/Sources/EmulatorViewController.m:56`, CONFIRMED). The self-test labels a
  successful RWX map "dynarec-ready".
- The A9 predates **APRR** (A11) and **PAC**/**PPL** (A12), so there is no
  per-thread hardware W^X to toggle and no pointer authentication to satisfy
  (`ARCHITECTURE.md`, CONFIRMED as the project's stated position; INFERRED as
  silicon fact, though it is well established).
- The device is jailbroken, and the jailbreak is what sets `CS_DEBUGGED`.

So on the intended host, the memory story is genuinely as simple as
`mmap(RWX)` → emit → cache-maintain → branch to it.

### 8.2 If CS_DEBUGGED is *not* set

Be precise about this, because it is the difference between "ship it" and "there
is no JIT".

**What happens (INFERRED, not tested on this device):** the `mmap` call with
`PROT_EXEC` may well *succeed* — the app's own self-test would print
"dynarec-ready" — and the process is then killed with a code-signing violation
the first time execution reaches an emitted instruction, or `mprotect` adding
`PROT_EXEC` to a written page fails with `EPERM`. **The current self-test is
therefore not sufficient evidence that the JIT will work**, and step one of J2
is to extend it: emit a three-instruction function (`mov w0,#42; ret`), call it,
and print the result. That is the real readiness check, it is twenty lines, and
it should be added *now*, long before any JIT, so the answer is known.

**What to do about it, in order:**

1. **Ask for it.** Dopamine/palera1n expose a "JIT enable" path (`jbdswDebugMe`
   / `libjailbreak`'s `jb_enable_jit`), which sets `CS_DEBUGGED` on the calling
   process. This is the intended mechanism and the app should call it at launch
   and re-probe.
2. **Dual mapping (RW + RX of one memory object).** Create a Mach memory entry
   with `mach_make_memory_entry_64` and map it twice with `mach_vm_map` /
   `mach_vm_remap`, writing through the RW view and executing the RX view. This
   is the standard trick on APRR-era hardware, and **it does not solve this
   problem**: on iOS the obstacle is code-signing enforcement of *unsigned
   executable pages*, not the simultaneity of W and X. Without kernel-side
   relaxation, the RX view is still unsigned and still refused. State plainly:
   **on iOS there is no userspace trick that makes unsigned code executable; the
   jailbreak is a hard requirement, not an optimisation.** (INFERRED, strongly
   held; the dual-mapping approach is worth implementing anyway because it is
   what a future A12+ host will need, §12.)
3. **W^X flipping** (`mprotect` RW → RX around each emission batch) is the
   fallback *shape* on hardware that enforces W^X, and costs a syscall plus a TLB
   shootdown per batch. It is unnecessary on the A9 and, again, does not defeat
   code signing. Design the code-buffer allocator as a thin platform shim
   (`jit_buf_alloc / jit_buf_begin_write / jit_buf_end_write / jit_buf_commit`)
   so all three policies are swappable and the macOS CI runner (which needs
   `MAP_JIT` + `pthread_jit_write_protect_np`) is just another implementation.
4. **No JIT at all.** The interpreter after §1.5 is ~5–6x faster than today's
   committed build. That is the floor, and it is why §1.5 is worth doing
   regardless of how the JIT question resolves.

### 8.3 Cache maintenance after emitting

Getting this wrong produces stale-code bugs that are non-deterministic,
unreproducible under a debugger, and will consume days. arm64 I-cache is not
coherent with the D-cache, and the required sequence for a range that has just
been written is:

```
    for each line:  dc  cvau, xAddr     // clean D-cache to Point of Unification
    dsb   ish
    for each line:  ic  ivau, xAddr     // invalidate I-cache to PoU
    dsb   ish
    isb
```

with the line size taken from `CTR_EL0` (`DminLine`/`IminLine`), not assumed —
though both are 64 bytes on the A9 (GUESS).

**Recommendation: do not hand-roll it. Call `sys_icache_invalidate(void*,
size_t)` from `<libkern/OSCacheControl.h>`** (CONFIRMED available on iOS), which
performs exactly the above. `__builtin___clear_cache` is the portable
equivalent. Hand-rolled sequences are how projects end up with an `isb` missing
on one path.

Two places it is easy to forget:

- **Every chain patch** (§3.5) rewrites a single branch instruction in
  already-executed code. It needs the same maintenance for those 4 bytes. A
  patch that is not maintained will be seen for a while and then, after an
  unrelated I-cache eviction, suddenly take effect — the worst possible bug
  signature.
- **After a code-cache flush** (§3.7), maintain the whole arena before reusing
  it.

The `isb` is only needed on the thread that will execute the new code; there is
one guest thread, so no cross-core shootdown protocol is required
(**INFERRED**; it becomes relevant only if the emulator ever runs guest code on
more than one host thread).

### 8.4 Registers and conventions the platform owns

- **`x18` is reserved.** Never allocate it (§4.1).
- Keep `x29` a valid frame pointer so crash reports and `lldb` can walk through
  JIT frames. The cost is one register that was already spoken for.
- The 16 KB page size on arm64 iOS does not interact with the *guest's* 4 KB
  pages, but the code arena must be 16 KB-aligned.
- The app is fake-signed with `ldid -S` (CONFIRMED, `ARCHITECTURE.md`); adding
  entitlements later is a build-script change, not a redesign.

---

## 9. Validation

"It seems to boot" is not evidence. The plan is that **every JIT-executed block
is provably equivalent to the interpreter executing the same instructions**, and
that this is checked automatically, off-device, in CI.

### 9.1 The existing suite, with the JIT backend

`core/tests/test_arm.c` (the CPU suite, ~142 cases) runs unchanged against the
JIT by switching the engine. Every case must pass with:

- the JIT enabled,
- the JIT enabled with a **one-block code cache** (forces retranslation of every
  block, exercising the translator on every execution),
- the JIT enabled with **all encodings denied** (proves the `arm_step` fallback
  path is transparent).

This is the cheapest possible signal and it must be green before anything else
is attempted. It runs on the macOS arm64 CI runner; on x86 CI hosts the JIT
compiles out and the suite runs interpreted, preserving the project's
"sub-second tests on the dev box" property (which the JIT otherwise costs us —
see §13).

### 9.2 Lockstep differential execution

The core tool. Two complete machines, `A` (interpreter) and `B` (JIT), created
identically and loaded identically, advanced in lockstep:

```
  loop:
    n = jit_run_one_block(B)          /* returns instructions retired */
    for (i = 0; i < n; i++) arm_step(&A.cpu)
    compare(A, B)                     /* architectural state + write log */
```

`compare` checks **all** of it, because a partial comparison finds bugs late:
`r[0..15]`, `cpsr`, `spsr[6]`, `bank_r13[6]`, `bank_r14[6]`, `fiq_r8_12[5]`,
`usr_r8_12[5]`, every `cp15` field, `excl_valid`, `excl_addr`, `abort_pending`
/`abort_fsr`/`abort_far`, `vfp_fpexc`, `vfp_fpscr`, and `cycles`.

Guest memory is compared by a **write log**, not by hashing 128 MB: each engine
records `(pa, size, value)` for every store into a ring, and the rings must match
exactly. This catches wrong values, wrong widths, wrong addresses and wrong
*order* at a cost of a few instructions per store, and it works even when the
two machines' device state diverges for legitimate reasons.

On the first divergence, dump: guest PC, block start VA/PA, the guest
disassembly of the block, the emitted host code, and the first differing field.
Divergences are otherwise nearly undebuggable.

### 9.3 Why the comparison is exact rather than approximate

The obvious objection to lockstep is that the two engines cannot agree, because
the interpreter ticks devices every instruction and samples interrupts every
instruction while the JIT does neither.

**That objection is removed by making the interpreter batch too** (§1.5, §7.6).
With both engines ticking only at sync points and sampling interrupts only at
sync points, and with device state advancing only inside `s5l8900_tick`:

- device state is a pure function of the sync-point schedule, which is shared;
- interrupt lines cannot change between sync points, so per-instruction sampling
  and per-block sampling give identical results;
- `cycles` and `tb_accum` advance identically.

So equality is *exact*, and any mismatch is a real bug. This is the reason tick
batching is not merely an optimisation but a prerequisite of the test strategy,
and the reason to land it in J1 rather than J5.

### 9.4 Instruction-level fuzzing

The suite tests what someone thought of. A generator produces random instructions
per encoding class into a single-instruction block, with randomised registers,
flags, and memory, and compares the two engines' resulting state. Weight it
towards the encodings §5 says are dangerous:

- every shift type × {immediate 0, 1, 31, 32-via-register, 33-via-register,
  255-via-register} × {S=0, S=1};
- RRX, and the immediate-rotate carry rule;
- `MSR` with all 16 field masks, in user and privileged modes;
- LDM/STM across all four addressing modes with and without Rn in the list, with
  `^`, with PC in the list;
- exception returns from every mode into both ARM and Thumb;
- accesses that straddle a page boundary, and accesses that fault.

Each generated case that diverges is minimised and added to `test_arm.c` as a
permanent regression, exactly as the four firmware-found bugs were.

### 9.5 Validating the TLB *before* the JIT exists

The software TLB (J1) is a pure cache, so it has a free oracle: build with
`IOS3VM_NO_TLB` and run the same boot. The two runs must produce **identical**
instruction traces, identical UART output, and identical final state. That test
should run over the full available boot on every CI build, and it is what would
catch the stale-mapping hazard of §6.1.

### 9.6 Torture modes

Correctness bugs in translation and invalidation hide because those paths run
once. Three switches, all usable with §9.2:

- **flush every N blocks** — retranslation, chain unlinking, arena reset;
- **deny a random 10% of encodings** — mixes JIT and interpreter at every
  boundary, exercising sync/reload;
- **invalidate on every store** — forces the §3.6 path constantly.

### 9.7 Trace replay, for cheap CI regression

Record from the interpreter, once, a compact trace of the first N million
instructions: `(block start VA, flags, instructions retired, 64-bit state hash)`
at every sync point. Store it as a fixture. CI then runs the JIT against the
fixture and asserts an exact match. This gives whole-boot coverage in CI without
shipping Apple firmware (the fixture contains hashes and addresses, not code)
— which matters, since `firmware/` is user-supplied and cannot be committed.

### 9.8 On-device parity

The app's self-test panel already runs the core on the phone. Add: the RWX
execution probe (§8.2), the CPU suite, and a short lockstep run. A JIT bug that
appears only on the A9 (cache maintenance, alignment, `x18`) will not be caught
anywhere else.

---

## 10. Staged plan

Each stage has an observable result and an honest expected multiplier. The
multipliers compound, and every one of them is stated relative to the previous
stage.

| | Stage | Observable | Expected speedup | Confidence |
|---|---|---|---|---|
| **J0** | **Build flags.** `CMAKE_BUILD_TYPE=RelWithDebInfo` (or an explicit `-O2`) for the core and tools; keep a debug configuration for bisecting. | `bootkernel -n 20000000` drops from 15.9 s to 5.3 s on the dev box. | **3.0x** | **measured** |
| **J1** | **Software TLB + batched device ticks + `arm_run(cpu, budget)`.** Interpreter only; no JIT, no new semantics. | Identical instruction trace and UART output to J0 (§9.5), at ~2x the speed. Real-kernel throughput on the dev box ~7–8 M insn/s. | **1.8–2.2x** | measured components (§1.2) |
| **J2** | **JIT skeleton.** RWX allocator + on-device execution probe, code arena, block cache, dispatcher, and a translator that handles *only* ARM data-processing and direct branches; everything else calls `arm_step`. Lockstep harness (§9.2) built here. | The CPU suite passes with the JIT engine; the boot reaches the same instruction with a bit-identical trace; the first native block executes on the phone. | **0.9–1.2x** — probably no faster, possibly slower | INFERRED |
| **J3** | **ARM completeness + memory fast path + block chaining.** All ARM encodings the interpreter implements, the §6.2 inline sequence, direct chaining, budget checks on back-edges only. | Boot to the current M4 stopping point, bit-identical, several times faster. | **3–5x** over J1 | GUESS, bounded by the mix in §1.3 |
| **J4** | **Thumb.** The 16-bit encodings and interworking. *Translation landed early, ahead of J3; PUSH/POP did not — see §0.* | The same boot again, now with 88.46% of retired instructions natively translated (measured, §0). | **2–3x** over J3 | INFERRED from the measured 68.95% Thumb share |
| **J5** | **Polish.** Dead-flag elimination (§5.3), indirect-branch target cache (§3.5), inline LDM fast path (§7.3), r8–r12 block-local caching, PC-store elision. | Profile-driven; each item gated behind a switch and validated by §9.2 with the switch on and off. | **1.3–1.7x** | GUESS |

### 10.1 What this compounds to

On the dev box, relative to the **committed** build's measured 1.26 M insn/s of
real kernel code:

- after J0+J1: **~3.5–4 M insn/s** measured-plus-inferred (the harness overhead
  is included; bare, ~7–8 M);
- after J5: **60–150 M insn/s (GUESS)**, i.e. 10–25 host cycles per guest
  instruction on a desktop core.

### 10.2 What gets us to interactive, and what does not

- **J0 and J1 do not.** They turn a 327x deficit into a 60x deficit. They make
  the *developer loop* better and they are prerequisites. They are not the
  answer to interactivity.
- **J2 does not, alone.** A partial-coverage JIT can be a net loss; expect and
  plan for that, and do not let it be read as failure.
- **J3+J4 are the whole game.** ARM completeness without Thumb leaves 69% of
  instructions on the slow path; Thumb without the memory fast path leaves half
  the cost in place. Neither is optional and neither pays off alone. **If only
  two stages are ever built after the skeleton, they are J3 and J4.**
- **J5 is worth ~1.5x and a lot of risk.** Do it last, behind switches, or not
  at all.

### 10.3 The honest end-state number

On the A9, at 1.85 GHz, expect **10–30 host cycles per guest instruction**
(INFERRED; the wide band reflects that this is a full-system emulator with a
software TLB, not a user-mode one, and that mobile cores are more sensitive to
I-cache pressure from emitted code than desktop cores):

- **60–180 M guest instructions/sec**
- = **0.15–0.45x** of the model's nominal 412 MHz
- = **0.25–0.7x** of how fast the real phone actually felt (§1.4)

**Plan on 0.25x. Do not promise 1.0x.** What 0.25x means in practice: the home
screen appears, icons respond to taps, and page-flip animations run at roughly
quarter speed. That is a demo and it is genuinely usable. It is not
indistinguishable from hardware, and any schedule or public claim that assumes
otherwise is built on a number nobody has measured.

The single biggest lever on that number is not in this table: it is whether J5's
inline LDM path and dead-flag elimination land, and whether the A9's small L1I
copes with the emitted code footprint. Both are measurable at J3, and the J3
measurement should be treated as the point at which this estimate is replaced
with data.

---

## 11. Recommendation: start now, or after M5?

> **Do J0 today. Do J1 next, before or during M5. Build the lockstep harness
> (§9.2) during M5, because the interpreter benefits from it too. Start the JIT
> proper (J2–J5) when the first SpringBoard frame renders — and solve M5's
> iteration-time problem with snapshot/restore, not with a JIT.**

### 11.1 Why the JIT waits

**Risks that waiting retires, all of them real:**

- **The interpreter is still growing.** Media/DSP, LDRD/STRD, VFP arithmetic and
  parts of the Thumb space still trap by design. Every instruction implemented
  while the JIT exists is implemented **twice** and tested twice. M5 will add
  more (SpringBoard's userland is more instruction-diverse than the kernel).
- **The device model is still moving.** LCD, multitouch, the FTL, and the
  network device of `networking.md` all change the memory map, and §6's TLB and
  §3.6's invalidation are coupled to what counts as RAM.
- **The JIT destroys the debugging tooling that has found every bug so far.**
  Four genuine emulator bugs were found by real firmware plus per-instruction
  tracing, symbol attribution, and first-abort reporting (`ROADMAP.md`,
  CONFIRMED). Under a JIT there is no per-instruction hook. Debugging M5 with a
  JIT in the loop means debugging two things at once, and the JIT is the one
  that produces impossible symptoms.
- **A moving oracle is a bad oracle.** The differential harness compares against
  the interpreter; every interpreter change is a JIT change (§5.4). Freezing the
  interpreter's semantics at "SpringBoard renders" makes the JIT a
  translation problem instead of a chase.

**Risks that get worse by waiting: almost none — provided two things are done
now.** The only genuinely expensive retrofits are (a) the software TLB, because
the JIT's fast path *is* the TLB and building it late means building the
interpreter's memory path twice, and (b) the interpreter API the JIT needs
(`arm_run(cpu, budget)`, sync points, batched ticks). Both are J1, both are
behaviour-preserving, both are worth doing on their own merits, and both are why
the answer is "J1 now" rather than "nothing now".

### 11.2 The trap in "wait for M5", and the actual fix

There is a real counter-argument and it should be stated rather than dodged. A
boot to SpringBoard is **GUESS 5–30 billion instructions** (a real device boots
in 30–60 s at ~250 M insn/s). At J1's ~7–8 M insn/s that is **10–60 minutes per
boot**. That is an intolerable edit-run loop, and it is the argument for
building the JIT during M5 rather than after it.

It is the wrong conclusion, because **even at the JIT's optimistic end (150 M
insn/s) a full boot is still 30–200 seconds**, and at the pessimistic end it is
minutes. The JIT does not fix the M5 iteration loop; it only makes it less
awful.

**Snapshot/restore does fix it. This is no longer a proposal — it shipped in
`95eaf8b`**, and the prediction held.

`core/src/snapshot.c` serialises the whole machine: the CPU including all banked
registers and SPSRs, the full CP15, the exclusive monitor, VFP, RAM, NOR, both
VICs, the timer, power, CLCD, and every stub window. The format is
`magic|version|len|flags → GEOM CPU MACH RAM NOR STUB END → FNV-1a-64`.

**Measured:** cold to 900 M instructions is **140 s**; restore-at-200 M and run to
900 M is **34 s**. Continue-to-400 M and restore-at-200 M-then-run-to-400 M produce
snapshots identical across all 432,618,045 bytes.

Three design notes, because the interesting problem was not serialisation but
*making field-omission hard* — a snapshot that silently drops one register
produces a divergent boot that costs days:

- Every field is named exactly once, in a visitor whose macros dispatch on
  COUNT/SAVE/LOAD, so save and load are the same list in the same order and
  cannot drift.
- A `_Static_assert` on `sizeof()` of every snapshotted struct names the visitor
  to extend, so adding a field to `arm_cpu_t` breaks the build instead of
  silently escaping the format.
- RAM is classified per 4 KB page (zero / uniform / raw) rather than
  dirty-tracked, because a write barrier would have to catch every bus write,
  loader path and future DMA model — and one missed path is exactly the silent
  divergence this exists to prevent.

It passed first try, which is when a test deserves suspicion, so it was
negative-controlled: dropping `timer.ticks` from a visitor gives 58252 vs 29126,
dropping `tb_accum` gives 176000000 vs 88000000 — both caught. That control also
exposed a trap now baked into the harness: **comparing only the two snapshot
files is insufficient, because a field the format never stores is absent from
both sides and cancels out.** `tools/snapboot` therefore diffs a machine-derived
report as well.

One caveat that matters when using it to debug: the host-side diagnostics — trace
ring, milestone hit counts, sampled profile, hot-page log — are **not** machine
state and are not restored. They start fresh and cover only the window since the
restore. Instruction indices are absolute either way.

**That is the recommendation's load-bearing claim, and it survived contact: the
thing that unblocks M5 is snapshotting, and the thing that unblocks
*interactivity* is the JIT. They solve different problems and should not be
confused for one another.**

### 11.3 The trigger to start early anyway

Start J2 before SpringBoard renders **only if** both of these become true:

1. snapshot/restore exists and the M5 loop is *still* dominated by emulation
   speed (i.e. the post-snapshot workload itself is billions of instructions —
   which would mean SpringBoard's own startup, not the kernel's); and
2. the interpreter's instruction coverage has been stable for several weeks,
   so the double-implementation cost has fallen.

Absent both, the sequencing above stands.

**Where that stands now.** Condition 1's first half is satisfied — snapshot/restore
exists — but its second half is not: the M5 loop is currently dominated by a
34-second restore-and-continue, not by emulation throughput, and the frontier is
a single mis-hashed page rather than billions of instructions of userland.
Condition 2 is plainly false; the last session alone added the CPUID registers,
`CPSR.A` on abort entry, and the User-mode `CPS` fix, and named four more
interpreter gaps it deliberately did not close. So the trigger has **not** fired,
and the J2 foundation being merged does not mean it has. J2 is parked on purpose.

---

## 12. What survives the ARMv7 / iOS 6 port

Assume the later port targets the S5L8920 (Cortex-A8, ARMv7) running iPhone
OS 6. **GUESS: ~75% of this design is unchanged.**

**Survives unchanged:** the layer-over-interpreter rule (§2); block cache, keying
and dispatch (§3) — indeed ARMv7's proper ASID in CONTEXTIDR makes the `ctx`
field *more* natural than it is on ARMv6; invalidation (§3.6); the register
mapping (§4) — ARMv7 has the same 16 registers and the same banking, plus a
Monitor mode we will not use; flags (§5); the software TLB and emitted fast path
(§6); interrupts, aborts, tick accounting (§7); cache maintenance (§8.3); the
entire validation strategy (§9).

**Needs real work:**

- **Thumb-2.** The 32-bit Thumb encodings roughly double the decoder, and iOS 6
  userland is Thumb-2 throughout. This is the single largest item.
- **IT blocks.** ARMv7's `IT` makes up to four following Thumb instructions
  conditional, with the condition state living in CPSR[15:10,26:25]. It
  interacts with block splitting (an `IT` block must not be split), with flag
  liveness (§5.3), and with exception entry mid-`IT`. Expect this to be the
  fiddliest correctness work in the port.
- **VFP/NEON.** ARMv6 userland of the 3.x era got by without FP arithmetic
  (the interpreter models only the VFP *system* registers today). iOS 6 userland
  will not. That is a whole FP semantics module before the JIT question even
  arises — and it belongs in the interpreter first, per §2.
- **Memory-model instructions.** `DMB`/`DSB`/`ISB` decode to no-ops on a
  single-core model, but must decode.
- **MMU details.** Still short-descriptor, but with TEX remap, the XN bit, and
  ASID-tagged TLB entries — the `ctx` tag becomes the ASID, and TLB invalidation
  becomes ASID-scoped rather than global.
- **A newer host, possibly.** If the port also moves to an A12+ device, §8's
  memory story changes completely: APRR/`pthread_jit_write_protect_np`, PAC on
  return addresses, and PPL. This is why §8.2 insists the code-buffer allocator
  is a four-function shim — that shim is the entire cost of a host change, and
  it is cheap to build now and expensive to retrofit.

---

## 13. Risks, in order of how likely they are to hurt

1. **The software TLB goes stale.** The highest-probability new bug in the whole
   plan, because it makes today's *correct* CP15 c8 no-ops load-bearing (§6.1).
   Symptom: a guest reads through a mapping the kernel already changed — which
   looks exactly like an emulator bug anywhere else. *Mitigations:* flush
   aggressively (whole TLB on any maintenance op), and §9.5's TLB-vs-no-TLB
   differential run on every CI build. **This risk arrives at J1, before any
   JIT.**
2. **Shifter carry-out and A64's differing logical-op flags (§5.2).** Exactly the
   nightmare the brief names. Silent, rare, and it corrupts a lock or a
   comparison thousands of instructions before anything visible happens.
   *Mitigations:* §9.4's targeted fuzzing of every shift form × amount ×
   S-bit combination, before J3 ships; and no `ANDS` fast path until that fuzzer
   is green.
3. **`CS_DEBUGGED` turns out not to be set, or not to be obtainable.** Would end
   the JIT entirely on this host (§8.2). *Mitigation:* it is cheap to find out —
   add the emit-and-execute probe to the existing self-test panel **now**. The
   current RWX-mmap probe is necessary but not sufficient and should not be
   treated as an answer.
4. **Cache maintenance missed on chain patches (§8.3).** Non-deterministic stale
   code, the hardest possible bug signature. *Mitigation:* one function performs
   every write to the code arena and no other path may; `sys_icache_invalidate`
   rather than hand-rolled sequences; a torture mode that patches and re-patches.
5. **The JIT cannot be tested on the Windows dev box.** This directly attacks the
   project's best property — sub-second local test loops (`ARCHITECTURE.md`).
   The JIT is arm64-only; local `ctest` will exercise only the interpreter.
   *Mitigations:* keep the JIT strictly optional at compile time so local
   testing is unaffected; put the JIT suite on the macOS arm64 CI runner the
   project already uses; and accept that JIT bugs have a slower loop, which is
   itself an argument for the sequencing in §11.
6. **The end-state speed disappoints (§10.3).** 0.15x rather than 0.45x is a
   different demo. *Mitigation:* measure at J3, on the phone, and revise the
   estimate publicly rather than quietly.
7. **Code-cache footprint on a 2 GB device.** 64 MB of RWX plus 128 MB of guest
   RAM plus a Metal framebuffer, under jetsam. *Mitigation:* the increased-memory
   entitlement is already carried; the flush-everything policy bounds growth;
   measure block counts at J3 against §3.7's guess.
8. **Divergence debugging is expensive even with the harness.** A mismatch 4
   billion instructions into a boot is a bad day regardless. *Mitigation:*
   §9.7's trace fixtures make the *first* divergence cheap to find, and
   snapshot/restore (§11.2) makes it cheap to reproduce.

---

## 14. What could not be verified here

Stated plainly, so none of it is mistaken for fact:

- **All throughput numbers are desktop x86 numbers.** No measurement in this
  document was taken on the A9. Every A9 figure is INFERRED or GUESS.
- **The 6.5 M insn/s figure this project quotes was not reproduced directly.**
  What was reproduced is that the committed build runs real kernel code at
  1.26 M insn/s and the same code at `-O3` runs it at 3.77 M insn/s *including*
  `bootkernel`'s heavy per-instruction instrumentation, with synthetic loops at
  8–14 M/s. 6.5 M/s for real code in an optimised, uninstrumented build is
  consistent with all of that, but the provenance of the original number should
  be confirmed before it is used in any comparison against a JIT number.
- **Whether unsigned executable memory actually executes on the target phone.**
  The app probes `mmap` success and `CS_DEBUGGED`; neither proves execution
  (§8.2). This is the cheapest high-value experiment available and it has not
  been run.
- **A9 cache line sizes and L1 capacities** are assumed 64 B / typical; read
  `CTR_EL0` rather than trusting this.
- **Instruction mix beyond 20M instructions.** The measured mix is early kernel
  init (`vm_page_init`, `zalloc`, `OSUnserializeXML` dominate the profile).
  Userland and SpringBoard will be *more* Thumb-heavy and more FP-heavy, which
  strengthens the J4-before-J5 argument but was not measured.
- **How many instructions a boot to SpringBoard actually takes.** The 5–30
  billion figure is arithmetic on a 30–60 s real-device boot, not a measurement.
  It is load-bearing for §11.2 and should be replaced with a real number as soon
  as a boot gets far enough to extrapolate from.

# Diagnosing a wedge

The same procedure has repeatedly found the bug: ADMFMC, the MBX GPU driver, the
IORTC wait, the TTBR abort storm, the post-SDIO silence, launchd's failing page
hash, the VFP restore, and the ARMv5TE DSP multiply stop. It was rediscovered
from scratch too many times. This document is that procedure, written down.

The shape of the problem is always the same. The boot stops making progress, and
it does so **without panicking** — a panic would tell you where you are. What you
have instead is a machine that is still retiring hundreds of millions of
instructions and getting nowhere, and a boot log that ends in the middle of
something unrelated.

Four tools answer four different questions. Use them in this order.

---

## 0. The recipe everything below assumes

```sh
build/core/bootkernel firmware/kernel.macho \
    -d firmware/devicetree.bin \
    -c "debug=0x8 serial=1 nand-enable-adm=0" \
    -r firmware/rootfs.img -R 512 \
    -n 400000000
```

`-R 512` is not cosmetic: `arm_vm_init` hardcodes `virtual_avail = 0xe0000000`,
so advertising more than 512 MiB at the documented virtual base makes
`zone_virtual_addr` index an empty `pv_head_table` and fault during
`zone_bootstrap` (commit `5625f5c`). The current machine also rejects a larger
physical aperture because `[0x08000000, 0x28000000)` is exactly 512 MiB and NOR
begins at `0x28000000`. Historical 768 MiB commands are rejected, not fallback
recipes. The
`nand-enable-adm=0` boot-arg is what stops `AppleS5L8900XADMFMC::start` from
panicking on a NAND controller we do not model. Two more workarounds are applied
automatically and printed in the header, so they are never invisible: the IORTC
wait is patched from 30 seconds to 0, and the `mbx` node's `compatible` string is
broken so the PowerVR driver does not match.

Large-input memory is part of the preflight. Current `bootkernel` proves all
static ranges are inside DRAM and pairwise disjoint, then streams the rootfs
through a retained source handle directly into its final guest address. It does
not hold a second roughly 445 MiB grown-image buffer. A source metadata change,
short read, overflow, overlap, or out-of-range placement fails before guest
execution.

Everything the tool did to the machine before the first instruction — segments,
patches, device-tree edits, ramdisk placement, `boot_args` — is echoed at the top
of the run. Read it. A wrong `-R`, a missing `-r`, or a device-tree patch that
silently did not apply produces a boot that fails hundreds of millions of
instructions later somewhere that looks unrelated.

---

## 1. "Where did it stop?" — the milestone table

The bottom of every run prints `CONSOLE-INIT MILESTONES`: a fixed list of
addresses taken from the kernel's own symbol table, each with a hit count and the
instruction index of its first hit, or `NEVER REACHED`.

This is the cheapest diagnostic in the project — one PC compare per entry — and
it is the one that turns "the boot is silent" into a sentence. Read it as a
**chain**, not as a set of counters: the last milestone that fired and the first
one that did not are the two ends of the interval containing the bug.

The list currently covers reset and console init, `bsd_init`, the pid-1 exec path
(`_load_init_program` → `_execve` → `_grade_binary` → `_load_machfile` →
`_ubc_cs_blob_add` → `_cs_validate_page`), the three verdict PCs *inside*
`cs_validate_page`, the first-level exception handlers, and `_panic` /
`_Debugger`.

Two patterns are worth naming because they recur:

- **A first-level handler count that climbs while `_unix_syscall` stays
  `NEVER REACHED`** means the new image is spinning on a fault rather than
  running. That is precisely what the launchd signature bug looked like: 12,542
  prefetch aborts, and not one SWI.
- **Probes inside one function, not just at its entry.** `cs_validate_page` can
  say no three ways, and they mean completely different things — so there are
  three milestones inside it (`no_hash_exit`, `hashing`, `bad_hash`). That
  distinction is what turned "code signing rejected launchd" into "the page we
  handed the kernel is not the page Apple signed", which is a bug on our side of
  the line. Adding a probe at a *branch* rather than at a symbol is routinely the
  difference between a policy argument and a located defect.

Adding a milestone costs one line in the `MILESTONES[]` table in
`tools/bootkernel.c` and a symbol lookup:

```sh
build/core/machoinfo firmware/kernel.macho -r 0xc013af54
```

---

## 2. "What is it spinning in?" — the profiler, and the kext symbolizer

`WHERE THE TIME WENT` samples the PC every 1,024 instructions and attributes it
through the kernel's `LC_SYMTAB`. For kernel code this names the function
outright.

For a **kext** it used to say nothing at all, and that was the single largest
time sink in this project's debugging history: five separate investigations began
with a profile that blamed one unsymbolized address. That is now solved
(`f105360`). `core/src/firmware/ksyms.c` parses `__PRELINK_INFO` and maps each
prelinked kext's `__PRELINK_TEXT` range to its `CFBundleIdentifier`, so an
address inside a kext resolves to `<bundle-id>+0xNNNN`, and the report gained two
sections: `TIME BY PRELINKED KEXT` and `HOTTEST INDIVIDUAL PCs`.

```sh
build/core/machoinfo firmware/kernel.macho -k        # the whole load map
build/core/machoinfo firmware/kernel.macho -r 0xc0778122
build/core/bootkernel firmware/kernel.macho -L       # map, then exit without booting
```

`-L` exits before machine init, so it is usable while a long boot is running in
another window.

**Know the limit of this tool before you rely on it.** Per-kext *function* names
are not merely unimplemented, they are impossible: each prelinked kext is an
`MH_KEXT_BUNDLE` carrying only `__TEXT`, `__DATA` and `LC_UUID` — the kernelcache
builder strips its `LC_SYMTAB` — and none of the kernel's 11,430 symbols fall
inside `__PRELINK_TEXT`. A kext address resolves to `bundle-id + offset` and can
go no further. The `HOTTEST INDIVIDUAL PCs` section exists precisely because the
profile bottoms out at the bundle id: `com.apple.driver.AppleMBX+0x122` is an
address you can disassemble, where "66.9% in AppleMBX" is not.

If the kext map fails to build, the boot log prints a banner saying why, with a
byte offset. A silently empty map is exactly what would waste the next cycle.

---

## 3. "Make the loop bearable" — snapshot and restore

Snapshot/restore (`95eaf8b`) reduced one historical desktop replay. It does not
establish the duration of the current frontier or every later iteration:

```sh
# save the machine at a checkpoint (up to 8 per run)
build/core/bootkernel firmware/kernel.macho -d ... -r ... -R 512 \
    -n 250000000 --snapshot-at 200000000 /tmp/at200M.snap

# then iterate from there
build/core/bootkernel firmware/kernel.macho -d ... -r ... -R 512 \
    -n 400000000 --restore /tmp/at200M.snap
```

Historical development-host/commit measurement: cold to 900 M instructions was
140 s; restoring at 200 M and running to 900 M was 34 s. Current source and
current-frontier timing must be measured again. The stronger current functional
result is a real-guest chain restored at 2.2, 2.4 and 2.7 B, with new checkpoints
at 2.4, 2.7 and 2.85 B, followed by a normal 2.9 B cap stop. The 2.4 B → 2.8 B
window observed a new `_execve` at 2,605,595,575 and `systemShutdown false`.
A diagnostic continuation from 2.85 B then stopped at instruction
2,944,340,624 on `0xe6cf3073`, ARMv6 `UXTB16 r3, r3`, in user mode.
After the complete paired-extend implementation landed, replaying that same
checkpoint cleared the stop, wrote a 2.97 B snapshot and reached a normal
2.98 B cap.

Three things about it will bite if you do not know them:

- **A restored run needs the same `-d` / `-r` / `-R` / `-p` / `-V` as the run that
  saved it.** `snapshot_load` checks geometry first and refuses with
  `SNAP_ERR_GEOMETRY` before a single register moves.
- **The trigger point is absolute.** `--snapshot-at` compares against the
  machine's own retired-instruction counter, which is itself part of the
  snapshot — so a run that restored at 200 M still fires `--snapshot-at 300000000`
  at the same instruction a run from zero would have.
- **Host-side diagnostics are not machine state and are not restored.** The trace
  ring, milestone hit counts, sampled profile and hot-page log start fresh and
  therefore describe only the window since the restore. Instruction *indices* are
  absolute either way. When you need a report that is a pure function of the
  machine, use `tools/snapboot`.

`snapboot` is the acceptance harness, and the reason it exists is worth
repeating: comparing two snapshot *files* is not sufficient, because a field the
format never stores is absent from both sides and cancels out. It therefore also
prints a machine-derived report to diff.

**Do not blindly extend the latest checkpoint yet.** Free pages fell from 2,004
at 2.45 B to 542 at 2.8 B and 317 at 2.9 B. The diagnostic continuation reached
a low of 97 pages at 2,934,505,472, recovered to 253 near the former opcode
stop, and ended at 214 at 2.98 B against a target of 250. That movement is evidence of reclamation, not proof that
the layout is safe. The RAM disk remains pinned guest memory even though its
host-side duplicate is gone. The storage audit has ruled out a simple external
aperture. The writable, range-gated md bulk-copy bridge, locked file adapter,
kernel UUID parser and atomic patch helper are now implemented and unit-tested;
wire them to a separately provisioned work image with snapshot coupling before app integration or an
unbounded continuation.

### WFI changes elapsed device time, not the instruction coordinate

XNU spends substantial time in the ARM1176 CP15 wait-for-interrupt form. The
machine now recognizes that exact privileged instruction and advances timer and
CLCD state to the earliest enabled VIC wake edge. It does **not** increment the
CPU's retired-instruction counter for the skipped wait. This distinction matters
when comparing an older snapshot or profile: the same amount of emulated time
can contain fewer idle instructions, while `--snapshot-at` still names an exact
retired-instruction coordinate. If no future wake is known, the callback makes
no speculative leap.

---

## 4. "What is it touching?" — the bus reports

Everything the guest does outside RAM is counted and attributed to a PC:

- `ALL NON-RAM PHYSICAL PAGES TOUCHED` — every distinct page, with the first PC
  that reached it. A driver talking to a device nothing models shows up here as a
  line, not as a boot that mysteriously takes forever. This is how the power-gate
  controller was found: one page absorbed 3,887,707 reads because a driver was
  polling a status bit that could never change.
- `HOT PAGE` — per-register counts, access sites, first and last accesses, and a
  40-bucket histogram over time. The histogram is what distinguishes "busy during
  init" from "still going at the end", which is the distinction between slow and
  stuck.
- `DISTINCT ABORT SITES` — every faulting PC with its FSR and FAR. `-T n` sets how
  many trace lines to print at the *first* data abort.

The guest clock is the tie-breaker between "slow" and "wedged": if the timebase
has frozen, the machine is in a loop that never takes an interrupt, and it is a
spin rather than progress.

For a user-mode undefined stop, do not read the numerical PC out of guest RAM as
if it were physical. The abnormal-stop report now translates the current VA
through the live MMU, prints the physical fetch mapping, and prints the fetched
instruction word. That is how the post-VFP stop became the exact statement
`0xe1630381` at VA `0x33dba604` = `SMULBB r3, r1, r3`, rather than "somewhere in
userspace". The complete related ARMv5TE family was then implemented and the
2.2 B checkpoint resumed through that address; chained restores reached the
2.9 B cap. The same report then identified `0xe6cf3073` as the next user-mode
blocker at instruction 2,944,340,624. Keep this translation in the report;
userspace has no kernelcache symbol to save a bad fetch assumption later.
The complete paired-extend implementation replayed through that exact address
to a clean 2.98 B cap, which is the required semantic confirmation.

---

## 4a. "Which code path did it take?" — count the instructions

This one is new, and it is the cheapest oracle in the toolbox: **an instruction
count between two milestones tells you which implementation ran.**

The launchd signature bug is the worked example. `cs_validate_page` was reaching
`bad_hash`, which by definition means our bytes were wrong — except two private,
untracked historical verifications reported that the disk image was perfect (a
UDIF verifier over every `blkx` CRC32, and an HFSX reader that reported
code-directory page hashes for all 155 signed Mach-Os and 6,731 code pages on
the volume; zero mismatches). Those tools and outputs are not in the public tree,
so this is not a reproducible current check.

The resolution came from arithmetic, not disassembly. `SHA1Transform` is ~2,262
Thumb instructions per 64-byte block, so hashing a 4096-byte page in software must
cost about **145,000** instructions. The observed `SHA1Init` → verdict interval was
**14,329** — an order of magnitude too few. Software SHA-1 had provably never run,
which meant something else computed that digest: `SHA1UpdateUsePhysicalAddress`
routes exactly-4096-byte buffers to a hardware engine whenever
`_performSHA1WithinKernelOnly` is non-NULL, and `IOCryptoAcceleratorFamily` had
matched and installed the hook. The engine at 0x38000000 is not modelled, so the
digest was fabricated from stub reads.

The general form is worth internalising:

> Estimate what the code you *think* is running should cost, then compare it with
> the interval the milestone table already gives you for free. An order-of-magnitude
> mismatch means a different code path ran — usually a hardware accelerator, a
> fast path, or a hook you did not know existed.

It costs one multiplication and no new tooling, and it distinguishes "our data is
wrong" from "our data never got there", which is a distinction that can otherwise
burn days.

---

## 5. The three lessons this procedure encodes

**Systemic beats device-specific — and the two feel identical from the log.**
Most of this project's wall-clearing has been device whack-a-mole: model one more
peripheral, un-match one more driver, patch one more wait. Two fixes were not
that shape at all. Honouring **TTBCR.N / TTBR1** (`e97934d`) and setting
**DFSR.WnR** (`85c4653`) were each a single architectural gap in the CPU, and
each one unblocked a dozen symptoms at once — the first unblocked a broad set of
IOKit drivers observed in that run, and the second reached userspace. Both
presented exactly as another
device problem would: a spin in an unsymbolized kext. The question worth asking
early, before modelling the next peripheral, is *"could one thing the CPU itself
gets wrong explain all of these?"*

**A bug invisible for 200 M+ instructions is usually only reachable from an
unprivileged or otherwise rare path.** The missing `DFSR.WnR` hid for ~230 M
instructions because privileged writes are *accidentally* satisfied by the
`AP=0b10` the kernel repairs the PTE to. Only an unprivileged access — `STRT` /
`LDRT`, or real user mode — could expose it, and the very first one the kernel
makes is the `copyout` of `"/sbin/launchd"`. Likewise, `CPS` being honoured in
User mode is a privilege escalation that a kernel-only boot can never reach.
So when something has been silently wrong for a very long time, do not look for a
rare *value*; look for a rare *mode* — user mode, abort mode, an unprivileged
access, a page-crossing access, a second CPU.

**"The profile blames one unsymbolized kext" is a solved problem.** It cost five
cycles. It should never cost another one. If you find yourself staring at a bare
address, run `machoinfo -k` before doing anything else.

---

## 6. What this tooling still cannot tell you

Stated so it is not rediscovered as a surprise:

- There is no per-instruction hook once the JIT is in the loop. Every bug found
  so far was found by real firmware plus per-instruction tracing; `docs/dynarec.md`
  §11.1 is where that trade-off is argued.
- The trace ring is a fixed depth (`-T`), so a fault whose cause is a million
  instructions upstream will not be in it. Snapshot before the interval and
  re-run with a milestone instead.
- Nothing here symbolizes *userspace*. Every tool above resolves against the
  kernelcache, so from the moment launchd retires its first instruction they go
  quiet — and that regime has now begun. It is a known gap, not a solved one.
- The abort-site table holds 48 entries. It now reports how many it dropped, but
  it still drops them; a table that silently truncated read as "these are all the
  abort sites", which is exactly the wrong thing to believe.
- The profiler's function table holds 1,024 entries and its hot-PC hash 8,192.
  Both print a `WARNING` when they overflow. **Read that warning before reading
  the percentages** — an earlier version of this profiler silently dropped
  everything past 64 entries and printed identical output at 200 M and 400 M
  instructions, which looked exactly like coverage.

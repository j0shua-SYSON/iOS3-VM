# Diagnosing a wedge

The same procedure has now been run five times, and each time it found the bug:
ADMFMC, the MBX GPU driver, the IORTC wait, the TTBR abort storm, and the
post-SDIO silence. It was rediscovered from scratch every time. This document is
that procedure, written down.

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
so advertising more than 512 MB makes `zone_virtual_addr` index an empty
`pv_head_table` and fault during `zone_bootstrap` (commit `5625f5c`). The
`nand-enable-adm=0` boot-arg is what stops `AppleS5L8900XADMFMC::start` from
panicking on a NAND controller we do not model. Two more workarounds are applied
automatically and printed in the header, so they are never invisible: the IORTC
wait is patched from 30 seconds to 0, and the `mbx` node's `compatible` string is
broken so the PowerVR driver does not match.

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
  running. That is exactly what the current M5 frontier looks like.
- **Probes inside one function, not just at its entry.** `cs_validate_page` can
  say no three ways, and only one of them is our bug — so there are three
  milestones inside it (`no_hash_exit`, `hashing`, `bad_hash`). Adding a probe at
  a *branch* rather than at a symbol is usually the difference between "code
  signing rejected it" and "the page we handed the kernel is not the page Apple
  signed".

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

A cold boot to the frontier is minutes. Snapshot/restore (`95eaf8b`) makes the
second and every subsequent iteration seconds:

```sh
# save the machine at a checkpoint (up to 8 per run)
build/core/bootkernel firmware/kernel.macho -d ... -r ... -R 512 \
    -n 250000000 --snapshot-at 200000000 /tmp/at200M.snap

# then iterate from there
build/core/bootkernel firmware/kernel.macho -d ... -r ... -R 512 \
    -n 400000000 --restore /tmp/at200M.snap
```

Measured: cold to 900 M instructions is 140 s; restore-at-200 M and run to 900 M
is 34 s.

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

---

## 5. The three lessons this procedure encodes

**Systemic beats device-specific — and the two feel identical from the log.**
Most of this project's wall-clearing has been device whack-a-mole: model one more
peripheral, un-match one more driver, patch one more wait. Two fixes were not
that shape at all. Honouring **TTBCR.N / TTBR1** (`e97934d`) and setting
**DFSR.WnR** (`85c4653`) were each a single architectural gap in the CPU, and
each one unblocked a dozen symptoms at once — the first started the entire IOKit
driver tree, the second reached userspace. Both presented exactly as another
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
- Nothing here symbolizes *userspace*. The moment launchd retires its first
  instruction, every tool above goes quiet, because all of them resolve against
  the kernelcache. That is a known gap in front of M5, not a solved one.

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
mkdir -p work
build/core/bootkernel firmware/kernel.macho \
    -d firmware/devicetree.bin \
    -c "debug=0x8 serial=1 nand-enable-adm=0" \
    --external-md firmware/rootfs.img work/rootfs-7e18-run01.img \
    -R 128 \
    -n 420000000
```

The external path is cold-boot only, its parent directory must exist, and the
work path must not exist. It accepts only the exact 7E18 kernel, device tree, and
rootfs identities, then creates a no-replace writable image and installs the
guarded md-strategy and raw-uio bridges. Default growth produces exactly
466,825,216 bytes
(445.199 MiB); budget at least 500 MiB plus logs and filesystem headroom on the
work volume. Any bridge failure, raw guest errno, undefined instruction, guest
`_panic`/`_Debugger` entry, or other halt exits nonzero. Raw guest errors use
status 11 and retain a separate exact diagnostic even if a later bridge failure
occurs. If the instruction cap lands inside native `_uiomove64`, active
continuations instead produce status 12; that is an incomplete measurement,
not a successful raw transfer. The work image remains after a later failure and its path cannot be
reused; archive/remove it deliberately or select a new filename. A cleanup
warning means a second large temporary may remain beside it and must be
inspected before retrying. No source firmware file is opened writable.

The faultable raw bridge reserves four 128 KiB bounce slots below
`topOfKernelData`; they are guest-RAM reservations, not extra host disk files.
Its zero-initialized 128 KiB host-memory tail keeps guard writes coherent with
later reads, while neither the 466,825,216-byte work image nor the immutable
source grows. External snapshots remain rejected because this overlay is not
serialized yet.

Checkpoint replay still uses historical direct-RAM mode:
`-r firmware/rootfs.img -R 512`. There, `-R 512` is not cosmetic:
`arm_vm_init` hardcodes `virtual_avail = 0xe0000000`,
so advertising more than 512 MiB at the documented virtual base makes
`zone_virtual_addr` index an empty `pv_head_table` and fault during
`zone_bootstrap` (commit `5625f5c`). The current machine also rejects a larger
physical aperture because `[0x08000000, 0x28000000)` is exactly 512 MiB and NOR
begins at `0x28000000`. Historical 768 MiB commands are rejected, not fallback
recipes. The `nand-enable-adm=0` boot-arg is what stops
`AppleS5L8900XADMFMC::start` from panicking on a NAND controller we do not model.
Three more workarounds are applied automatically and printed in the header: the
IORTC wait is patched from 30 seconds to 0, and the `mbx` and `sha1` nodes are
unmatched so the unmodelled PowerVR and hardware SHA-1 paths do not run. `-g` and
`-S` deliberately re-enable those known-broken paths for diagnostics;
external-md rejects `-K`.

Large-input memory is part of the preflight. External mode copies and hashes the
source through a buffer capped at 1 MiB; the media never occupies guest DRAM.
Direct mode proves all static ranges are inside DRAM and pairwise disjoint, then
streams the rootfs through a retained source handle directly into its final
guest address. A source metadata change, digest mismatch, short read, overflow,
overlap, or out-of-range placement fails before guest execution.

Everything the tool did to the machine before the first instruction — segments,
patches, device-tree edits, storage mode, `boot_args` — is echoed at the top of
the run. Read it. A wrong `-R`, a missing root source, or a device-tree patch
that silently did not apply produces a boot that fails hundreds of millions of
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
    -n 250000000 --snapshot-at 200000000 work/at200M.snap

# then iterate from there
build/core/bootkernel firmware/kernel.macho -d ... -r ... -R 512 \
    -n 400000000 --restore work/at200M.snap
```

Keep snapshots on the workspace/non-`C:` volume. They can occupy hundreds of
MiB, and atomic replacement creates a same-directory temporary, so replacing an
existing checkpoint can briefly require room for both files.

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
- **Checkpoint requests fail loudly.** Counts are parsed as strict unsigned
  64-bit values. A target before the restored count or beyond `-n` is rejected;
  a target equal to the current count is saved before stepping; a terminal CPU
  stop that leaves any future target unfinished produces the normal diagnostic
  report and exits with status 5. `-L` rejects restore/checkpoint options instead
  of silently ignoring them.
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
stop, and ended at 214 at 2.98 B against a target of 250. That movement is
evidence of reclamation, not proof that the direct-RAM layout is safe. The
external-md path avoids pinning that disk in guest memory. Its pre-raw-bridge
run reached the first 32 KiB `/dev/rmd0` read at 402,741,536 instructions with
21,187 free pages (82.76 MiB) after 6,715 strategy reads and no bridge failure.

Run03 crossed that guard and reached 420,000,000 instructions, but fsck exited
with signal 8. Run04's per-request diagnostics found the same pair during
fsck's `-p` and `-fy` passes:

```text
#1 seg=5 rw=0 resid=32768 offset=0
   fault=0x01001000/pa=0x00000000/fsr=0x00000807
#2 seg=5 rw=0 resid=32768 offset=0x1bd30000
   media_end=0x1bd33000
```

The first is a guest write-side demand-page/COW requirement: a raw-device read
must copy into a user page that is not resident yet. Do not weaken page-table
permissions or fabricate a host translation. The second has 12 KiB inside the
work image and a 20 KiB adjacent tail. The closest public XNU `_mdevrw` calls
`uiomove64` once without a logical EOF bounds check, so an immediate `EINVAL`
is not faithful either.

The implemented fix exact-patches the four bytes to
`svc #0xe3; svc #0xe4`. The first SVC uses `ARM_SVC_REDIRECTED` to enter exact
Thumb `_uiomove64` at `0xc0128d14` when direct translation fails. One of four
128 KiB slots is selected by entry kernel SP, and the second SVC validates the
native completion before returning to `_spec_read`/`_spec_write`. The adjacent
128 KiB tail is a zero-initialized coherent memory overlay and is never added
to either disk file.

Run05 met those acceptance conditions with a fresh work image. It reached its
430,000,000-instruction cap with status 0 after `launchd`, `Running fsck on the
boot volume...`, and `/dev/md0 on / (hfs, local, noatime)`. Both raw requests
completed: two reads, two native `_uiomove64` redirects, two checked
completions, zero guest errors, and zero pending continuations. The raw split was 45,056 bytes
from media plus 20,480 bytes from the coherent guard. Across both bridges the
run completed 6,901 reads (28,295,168 bytes), one 512-byte write, and zero
failures; strategy I/O accounted for 6,899 reads and the write. The work image
remained exactly 466,825,216 bytes, and all source firmware hashes remained
unchanged.

The low-water sample was 20,820 free pages (81.33 MiB) at instruction
425,852,928. `_execve` recorded 11 hits and `_load_machfile` recorded 6, so the
clean cap was progress past the raw/fsck blocker, not evidence that SpringBoard
started.

Run06 then extended the same path to a clean 1,000,000,000-instruction cap with
empty stderr. It retained launchd, fsck, and the root mount, printed both
`mDNSResponder[14]` Seatbelt lines and `systemShutdown false`, and recorded
10,004 external reads (40,994,304 bytes), 27 writes (107,008 bytes), and zero
failures. Strategy accounted for 10,002 reads and all 27 writes; raw I/O
remained two reads, two native redirects, two completions, zero guest errors,
and zero pending continuations. The raw media/guard split remained
45,056/20,480 bytes. Free pages reached a low of 17,221 (67.27 MiB) at
980,615,168 instructions. `_execve` remained at 11 while `_load_machfile`
advanced to 25. The work image stayed exactly 466,825,216 bytes and all source
firmware hashes remained unchanged. No SpringBoard frame was captured.

Run07 extended the same fresh 128 MiB path to a clean
2,000,000,000-instruction cap. The exit file contained 0, stdout was 234,838
bytes, and stderr was empty. The final PC was `0x3145ad4c` in USR mode
(`CPSR 0x20000010`); 731,259,769 instructions (36.6%) retired in USR mode.
`_execve` reached 12, `_load_machfile` 32, `_thread_bootstrap_return` 92,620,
and `_unix_syscall` 58,166. The launchd/fsck/root-mount,
`mDNSResponder[14]` Seatbelt, and `systemShutdown false` lines all remained.

The bridge reported 12,782 reads (52,372,992 bytes), 82 writes (325,120
bytes), and zero failures. Strategy accounted for 12,780 reads and every
write. Raw I/O remained two reads, zero writes, zero guest errors, two native
redirects, two checked completions, and zero pending continuations. It read
45,056 media bytes and 20,480 coherent-guard bytes, with no raw media or guard
writes. Final free memory was 13,000 pages (50.78 MiB); the low was 12,983 pages
(50.71 MiB) at instruction 1,836,056,576. The work image stayed exactly
466,825,216 bytes and all source firmware hashes remained unchanged.

Do not use run07 as a display check. Its framebuffer was disabled, and CLCD
status, interrupt mask, and scanning were all zero. The run proves additional
serial userspace execution, not SpringBoard startup or display-path operation.

At `df9dc7b`, hosted
[`core-tests` run 30004015881](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30004015881)
and
[`ios-build` run 30004015807](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30004015807)
also completed successfully. Continue with a new work filename and a higher
bounded cap before app integration. Snapshot backing-identity and overlay
coupling remain separate work.

### CLCD: configured is not scanning

When diagnosing a future display run, do not use the old `0x0d8..0x0ec`
`TIMING` labels. Those words are per-window auxiliary configuration pairs. The
`0x0e8` word also participates in `AppleH1CLCD` window updates. The actual N82
panel timing registers are `VIDTCON0..3` at `0x20c..0x218`; an iBoot-compatible
320x480 seed reads back as:

```text
VIDCON0   00000441
VIDCON1   00000008
VIDTCON0  00030303
VIDTCON1  000e0e0f
VIDTCON2  013f01df
VIDTCON3  00000001
```

For the production N82 handoff, `VIDCON0` means 54 MHz display clock divided
by five with scanout enabled, and `VIDCON1` preserves inverted-VCLK polarity.
`VIDTCON2` must match the requested geometry rather than a universal constant:
320x480 produces `0x013f01df`. The initial configuration should also show
`0x00001000` at `0x0d8`, `0x0e0`, and `0x0e8`, with `0x0dc`, `0x0e4`, and
`0x0ec` clear.

An enabled window proves only that its address, geometry, stride, and format
remain configured. Live scanout additionally requires controller start state,
the `CLCD_CTRL` global enable, and `VIDCON0` bit 0. Frame progression, frame IRQ
timing, WFI wake selection, and host frame publication must all stop when any
one of those gates is off. This prevents a stale window from masquerading as a
live panel.

These checks describe the corrected model, not a successful display boot.
Run07 disabled the framebuffer and reported zero CLCD activity. Run09 later
retained the corrected seed and captured a SpringBoard launch request, but not
spawn success, SpringBoard execution, or rendering.

For a display-focused CLI run, `-H 0x38900000` moves the bounded hot-page
register trace to the CLCD page; `-H` rejects unaligned or out-of-range physical
pages. The final report also counts every observed instruction-entry PC in the
`AppleH1DisplayDrivers` and `AppleMerlotLCD` executable ranges rather than
relying on the one-in-1,024 profiler. Low physical aliases count only before the
MMU is enabled, so a userspace PC in the DRAM-sized numeric aperture cannot
become false display-driver evidence. The report retains a bounded exact-PC
histogram and every observed outside-to-inside entry edge with instruction
index, LR, and CPSR; any unretained observations or edges are counted
explicitly. These remain observations at instruction entry, not proof of
retirement or successful driver startup.

The process-lifecycle section retains the newest 256 fork, exec, spawn, wait,
kill, exit, `_exit1`, and `_psignal` entries. Exec/spawn pathnames are copied
through the caller's live user MMU mapping with user permissions, one byte at a
time and within a 256-byte bound. An exact SpringBoard pathname is labeled as an
attempt only; it does not prove syscall success, process execution, or a frame.

With `-F`, `firmware/screen.ppm` is invalidated before guest execution. A final
PPM is published only from a currently running CLCD and active, valid RGB
window, and the report labels it `ALL BLACK` or `NONBLACK`. Either result is
scanout evidence only. A recognizable SpringBoard frame, together with
lifecycle and driver evidence, is still required for boot-completion proof.

### Run08 display diagnostic

Run08 used a fresh 128 MiB external-md work image with
`-F -H 0x38900000`. The harness reached its 600,000,000-instruction
cap with `stopped ... OK`; final PC was `0xc017056c` (`_SHA1Init+0xc4`) and
stderr was empty. The wrapper's exit-marker file was accidentally empty, so do
not report an OS process exit status for this run.

The exact code-range counters were:

```text
AppleH1DisplayDrivers  675 hits  first 126211220  last 201032245
AppleMerlotLCD         409 hits  first 209372737  last 211410011
CLCD page accesses      0
```

Those PC observations prove only that the CPU reached addresses in both bundle
ranges. They do not prove retirement or that `AppleH1CLCD::start` completed.
With no guest CLCD MMIO, seeded configuration remained intact while guest-time
ticking advanced IRQ status and the frame counter: status 1, mask 0, scanning
1, `CLCD_CTRL = 0x41`, `VIDCON0 = 0x441`, `VIDCON1 = 0x8`, window 0 active,
running 1, and 386 frames.

The resulting frame was `NONBLACK` only in the literal counter sense. It had
384 nonzero RGB bytes: 128 white pixels in one 8x16 block at the top-left, with
every other pixel black. Do not treat that block as a SpringBoard frame or as
evidence of Apple-display-driver-driven output.

The lifecycle ring retained 70 events with zero pathname-copy failures.
Launchd, fsck, and the root mount were present; service spawns progressed
through `/usr/sbin/notifyd` at instruction 586,776,479. Exact SpringBoard path
attempts were zero. User mode retired 44,274,420 instructions (7.4%), and free
pages reached a low of 19,260 (75.23 MiB).

Storage remained clean: 8,059 reads (33,034,752 bytes), 16 writes (61,952
bytes), zero failures, two raw redirects, two completions, zero pending
continuations, and zero raw guest errors. The source hashes were unchanged.

Run09 supplied the longer bounded run described next. Zero MMIO remained
important, but alone it did not identify the exact blocker.

### Run09 SpringBoard launch-request diagnostic

Run09 used a fresh display-enabled 128 MiB external-md work image and a
2,000,000,000-instruction cap. The harness reported `stopped ... OK` and stderr
was empty. The wrapper's OS process exit marker was unavailable, so do not
report a host exit code. User mode retired 729,934,906 instructions (36.5%).
The free-page low was 12,976 pages (50.69 MiB) at instruction 1,829,371,904.

The lifecycle ring retained 120 events. It captured one exact stock
SpringBoard pathname in a `posix_spawn` attempt at instruction 635,280,837.
That event proves only that the caller requested the path. It does not prove
the syscall returned successfully, that a child existed, that SpringBoard
retired an instruction, or that it rendered. The one pathname-copy failure was
a separate later event.

The exact display diagnostics were:

```text
AppleH1DisplayDrivers  687 hits  first 126211220  last 1571737384
AppleMerlotLCD         409 hits  first 209372737  last 211410011
SPI0                    13 early platform writes
CLCD page accesses       0
```

The only late H1 activity was six two-instruction callbacks.
`AppleMerlotLCD` did not advance beyond run08, SPI0 never progressed beyond
early platform writes, and no CLCD MMIO was recorded. Seeded scanout reached
589 frames, but the final PPM was byte-identical to run08: exactly 128 white
pixels in an 8x16 top-left block and every other pixel black.

Storage and memory remained stable: 12,798 reads (52,438,528 bytes), 82 writes
(325,120 bytes), zero bridge failures, and unchanged source-firmware hashes.
The next diagnostic should capture the `posix_spawn` return/outcome and child
lifetime while retaining the lifecycle, SPI0, CLCD, and frame checks.

The harness now arms a bounded raw-return probe only when that exact stock
pathname is copied successfully. It discovers and validates the unique
`MOVS pc,lr` gate in `_thread_exception_return`, then accepts only the matching
SVC-to-USR transition with the same TTBR/FCSE/context identity, kernel and user
thread-ID registers, SVC stack, user stack/link register, ARM/Thumb state, and
resume PC. A later same-thread SWI closes an unresolved generation so another
call through the same libc wrapper cannot be misattributed. The report retains
raw `r0`, `r1`, `CPSR.C`, and separate transition/resume instruction indices.
Run09 predates this probe; a new real-firmware run is required before it can
answer whether the recorded spawn returned success or an errno.

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

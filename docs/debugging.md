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
external-md rejects `-K`. Run16 proves that the new I2C/PCF50635 model lets PMU
start succeed with live slave traffic, but it retained this IORTC patch. Direct
`IORTC` publication needs a one-patch diagnostic option or clearly identified
targeted build; the existing `-K` switch is not that experiment.

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

### Run11 and the SETEXEC correction

Run11 reached a clean 700,000,000-instruction cap with durable direct log
redirection and empty stderr. It repeated SpringBoard at 635,280,837, then
recorded BTServer at 637,448,889. The old wrapper-return probe remained pending
and no associated `_thread_resume` entry appeared. Run11 did not decode the
spawn flag, so those absences alone are neither failure nor success evidence.

[Matching-era launchd](https://github.com/apple-oss-distributions/launchd/blob/launchd-329.3/launchd/src/launchd_core_logic.c#L3640-L3836)
forks one child per job, sets `POSIX_SPAWN_SETEXEC`, and calls `posix_spawn`
with a null PID output. Exact local disassembly is the authority for the shipped
xnu-1357.5.30 binary; [Apple's older public XNU control-flow
analogue](https://github.com/apple-oss-distributions/xnu/blob/xnu-1228.15.4/bsd/kern/kern_exec.c)
helps interpret it. Run11's one-to-one fork/spawn trace strongly predicts the
same SETEXEC shape; run15 later confirmed flag `0x0040`.
SETEXEC bypasses vfork. On success the fork child is
exec-replaced and enters the new image rather than returning to launchd's old
wrapper; on failure it returns an errno. The lifecycle ring's 19 late forks and
19 service spawns are consistent with distinct per-job children. Their cloned
address spaces explain the repeated wrapper PC and argument virtual addresses,
but run11 did not record enough identity to assign either process directly.

The bounded probe used by run15 is fail-closed:

1. At the exact SpringBoard pathname SWI, it re-walks
   `thread+0x34c → task`, `task+0x1c4 → proc`, `proc+8 → PID`, and
   `thread+0x43c → uthread`. It decodes the spawn attribute descriptor and
   requires flag `0x0040` for SETEXEC.
2. Exact Capstone-verified instruction windows locate the
   `exec_activate_image` callsite, its `_load_machfile` call, and the
   `_posix_spawn` result epilogue. Function-entry phases require their exact
   caller LR. The SETEXEC proof requires image activation/load hits and zero
   `_vfork`, `_vfork_exit`, `_vfork_return`, or `_thread_resume` hits.
3. At the result epilogue, the preceding `mov r0,r4` makes `r0` the kernel
   outcome: zero is activation success and nonzero is the errno candidate.
   `CPSR.C` and a return to the old wrapper are not authorities for SETEXEC.
4. After zero, a user PC is only a candidate. Immediate IRQ/FIQ deflection and
   demand fetch faults defer it. The claim is committed after an `ARM_OK`
   interpreter step and matching task/uthread/effective-proc/PID identity,
   including a post-step retry when the new map was initially unreadable.
5. `_psignal` and `_exit1` match the exact entry proc pointer plus a freshly
   re-read PID. The first exit is terminal, preventing later proc-object reuse
   from being credited.

The vfork child-resume probe remains available for non-SETEXEC calls, where
`_thread_resume` is identity/lifetime evidence rather than success authority.
Run11 predates the new identity and epilogue fields, so its late
`_exit1(proc=e0381ca8)` remains unattributed.

### Run15 exact SpringBoard execution diagnostic

Run15 completed a fresh 2,000,000,000-instruction display-enabled cold boot
with `OK` and empty stderr. It confirmed live `POSIX_SPAWN_SETEXEC`, exact
image-activation and `_load_machfile` phases, and result `r0=0`. The replacement
process first retired an identity-validated instruction at 636,114,681 and
accumulated 37,134,545 address-space-keyed user instructions through
1,851,355,734. The exact process took 882 traced traps, never entered `_exit1`,
and ended scheduled out during a validated `mach_msg` SVC.

The first low-image PC was `0x34e8` at 1,519,973,164. A read-only HFSX/Mach-O
audit resolves it to the untouched stock SpringBoard binary's
`LC_UNIXTHREAD`/exported `start`; later low PCs resolve through Objective-C
metadata to genuine SpringBoard methods. The image's 291 embedded code-page
hashes all verify. This proves executable entry and subsequent application-code
execution, not a directly retained `UIApplicationMain` call or UI readiness.

Keep the evidence contracts separate:

- A committed exact-target user step proves instruction retirement under the
  revalidated process key; it does not prove a frame.
- A target/live-scanout mutation proves pixel-memory activity, not a
  recognizable screen; the PPM remains the visual authority.
- A task-local Mach port name or message ID is not a server identity without an
  independently established IPC mapping.
- Profiles produced before the context-aware classifier in `9e2bc3f` may have
  misclassified user PCs by applying a synthetic high alias. Use run15 or a
  newer trace for userspace-region claims.

Run15 retained the lifecycle, SPI0, CLCD, exact-process mutation, and PPM checks.
All framebuffer mutation counts were zero and the PPM remained the seed-only
8x16 white block. Run17 supplies the next localization step below; it does not
change the run15 SETEXEC proof.

### Run16 PMU/I2C and display-start diagnostic

Run16 was a fresh display-enabled external-md smoke run capped at 250,000,000
instructions. It exited with status 0 and empty stderr. No userspace instruction
retired by the cap, so it cannot say whether SpringBoard rendered or even
started in this run.

The machine now maps i2c0 and i2c1 at their real S5L8900 MMIO windows and drives
VIC0 IRQ21 and IRQ22. A PCF50635 PMU/RTC slave is attached to i2c0 at seven-bit
address `0x73`; controller, in-flight transaction, and PMU state are snapshot state and
have focused transfer, IRQ/NAK/W1C, RTC, malformed-state, and restore coverage.
The live diagnostic reported:

```text
i2c0 START events                 57
i2c wait-condition hits          44
i2c post-wait hits               44
PMU start-failure hits             0
AppleH1DisplayDrivers entries  10803
AppleMerlotLCD entries            948
CLCD page accesses              795r / 32w
```

The first-PMU-I2C-call entry and controller post-wait checkpoints were both
observed, but those aggregate counters do not associate a particular call with
a particular wait. The PMU's pre-I2C-parent and first-I2C checkpoints were hit,
while its start-failure checkpoint was not. Combined with the driver log and
exact/static control flow, this proves PMU start success and live PCF50635 bus
traffic. It does not prove `IORTC` publication because the ordinary 7E18
zero-timeout patch was still active. A one-patch diagnostic option or clearly
identified targeted build is needed; `-K` disables the whole patch table and is
rejected by external-md.

For display diagnosis, run09's “Merlot remained frozen at 409” is now historical
evidence, not the current blocker. Run16's control-flow audit proves both
observed Merlot start calls and H1 `start_hardware` returned success. The guest
also changed CLCD control and interrupt-mask state. However, the final PPM was
still the seed-only 8x16 white block. Successful observed Merlot `start` and H1
`start_hardware` returns are not equivalent to a userspace surface or a
SpringBoard frame.

The driver also accessed display-adjacent physical pages `0x39100000`,
`0x39200000`, and `0x39300000`. At the run16 checkpoint they were unmodelled
fidelity risks, and the accesses alone did not establish a blocker. Run18's
exact close/wait trace and the shipped interrupt path later supplied the
missing causal evidence for the optional TV-out chain, as recorded below.

Run17 kept this model and performed the full 2,000,000,000-instruction
experiment. Its later userspace/display boundary is recorded below. Run the
IORTC publication check separately once a diagnostic that disables only its
zero-timeout patch exists.

### Run17 local CAWindowServer/IOMobileFramebuffer diagnostic

Run17 completed a fresh display-enabled external-md cold boot through the
2,000,000,000-instruction cap with status `OK`. Exact SETEXEC activation
succeeded again and exact-process `_exit1` was not reached. The CLCD was active
at 320x480, but the PPM was byte-identical to the run15/run16 seed-only capture:
384 of 460,800 RGB bytes were non-zero.

The retained exact-target low flow establishes this ordering:

```text
SpringBoard UIApplicationMain call                 0x0000381e
UIKit registerForSystemEvents call / return        0x324a5098 / 0x324a509c
UIKit _startWindowServerIfNecessary call           0x324a50c8
UIKit [SpringBoard rendersLocally] call / return   0x324a5b84 / 0x324a5b88
SpringBoard applicationDidFinishLaunching:         0x0000a6f4  NOT OBSERVED
```

The return from `rendersLocally` carried `r0=1`. Run17 also reached
`_IOMobileFramebufferGetDisplaySize+0x18` at `0x3110d024`, with
`r0=0x0021c8c0` and LR `0x3123ef50`. Static disassembly resolves that LR to
`CA::WindowServer::IOMFBDisplay::update_framebuffer+0xbc`; the containing
constructor had advanced through its accepted `IOMobileFramebufferOpen` path.
At the run17 checkpoint, this placed the observed interval after
local-window-server selection and inside IOMobileFramebuffer setup, but before
SpringBoard's launch callback. Run15 reached the callback while run17 did not;
run17 alone did not distinguish timing from a model defect.

The most recent exact-process Mach episode carried request message ID 2816,
the IOKit `io_service_close` routine ID. The target switched out while the
receive path waited. H1 display-driver instruction entries were observed within
that same episode through instruction 1,873,360,702, followed by
`_wait_queue_assert_wait` at 1,873,361,179. Keep the attribution boundary
strict:

- message ID 2816 identifies the routine shape, not the task-local destination
  port, connection object, or service;
- the H1 code-range observation does not distinguish CLCD from TV-out or prove
  which userspace call initiated the episode;
- IOMobileFramebuffer's finalizer calls `IOServiceClose` at `0x3110dc1c`, but
  that was only a static candidate correlation until run18 observed its exact
  call/wait ladder around the Mach episode.

The exact 7E18 userspace map used for that run18 split is:

- UIKit `+[UIApplication _startWindowServerIfNecessary]` starts at
  `0x324a5b70`. After `rendersLocally`, it calls
  `[CAWindowServer server]` (`0x324a5ba0/0x324a5ba4`), sets renderer flags
  (`0x324a5bb4/0x324a5bb8`), obtains `displays`
  (`0x324a5bc4/0x324a5bc8`), counts them
  (`0x324a5bd4/0x324a5bd8`), selects index zero
  (`0x324a5bf0/0x324a5bf4`), obtains its bounds
  (`0x324a5c04/0x324a5c08`), and calls `_GSSetMainScreenInfo`
  (`0x324a5c40/0x324a5c44`).
- QuartzCore `-[CAWindowServer _detectDisplays]` spans
  `0x3125408c`-`0x312541c8`. Its indirect display-open call/return is
  `0x312540ec/0x312540f0`, and its `new_server` call/return is
  `0x3125411c/0x31254120`. The H1CLCD opener matches `AppleH1CLCD`, obtains
  the IOKit service, and constructs the H1 display.
- The IOMFB display constructor starts at `0x3123f5a0`. Its
  `IOMobileFramebufferOpen` call/return is
  `0x3123f6b8/0x3123f6bc`; `update_framebuffer` is
  `0x3123f6c8/0x3123f6cc`; and layer zero's default-surface call/return is
  `0x3123f6e0/0x3123f6e4`.
- Inside IOMobileFramebuffer, selector 8's display-size call/return is
  `0x3110d05c/0x3110d060`; its two raw outputs occupy `SP+8` and `SP+0x10`
  before conversion to cached floats at object offsets `0xa8` and `0xac`.
  Selector 3's layer-surface call/return is `0x3110d8e4/0x3110d8e8`,
  followed by `CoreSurfaceBufferLookup` at `0x3110d900/0x3110d904`.
  There is no separate string-backed `GetDisplayArea` routine in this
  framework image; `GetDisplaySize` is the observed geometry API.
- The IOMobileFramebuffer finalizer spans
  `0x3110dbcc`-`0x3110dc20`; it loads the connection at `0x3110dc18` and
  calls `IOServiceClose` at `0x3110dc1c`.

The console line `IOSurface: buffer allocation size is zero` occurs in generic
IOSurface initialization before the H1 path fills its descriptor. In this path
the surface remains non-null and H1 assigns width 320, height 480, row bytes
1280, pixel format `ARGB`, and allocation size `0x96000`. Do not suppress the
warning or promote it to the boundary without a failing return or an exact
caller correlation.

The three display-adjacent pages are no longer anonymous: `0x39100000` is the
H1 TV-out control/coefficient/DAC block, `0x39200000` is its mixer, and
`0x39300000` is its SDO block. Their relationship to the optional TV-out close
path is now exact; it does not make them a blocker for the already completed
primary CLCD construction.

#### Run18 result: optional TV-out close/swap wait

Run18 exercised the post-retirement UI checkpoints, newest-retaining Mach ring,
and late H1/Merlot edge ring in a normal display-enabled
2,500,000,000-instruction cold boot. It stopped at the configured cap with
`OK`, empty stderr, and no kernel panic/debugger entry. The source artifacts
were reverified after the run:

```text
kernelcache.release.s5l8900x  0D8CDB339D37CF37A1DB2638FFF79272ECD63A17764BF7666EFA1618725DF70C
DeviceTree.n82ap             4867C95FEDF544BDA2ECAA2626AE14C01A60D7771DC53FFE6FD3A6AAC8B8BA57
018-6494-014.dmg             C3251E7F092C939D5818E92086CB47680981CFB03731DE7B55D238C942EB5E82
```

Those are the immutable original source artifacts. Kernel and device-tree
compatibility edits apply only to loaded guest copies; the writable filesystem
is a fresh work-image copy, never the source DMG.

The exact target chronology is:

```text
SETEXEC result epilogue, r0=0                         609,608,299
first identity-validated replacement instruction     609,722,091
SpringBoard UIApplicationMain call                 1,828,280,094
[SpringBoard rendersLocally] returns YES           1,852,111,473
optional IOMFB finalizer                            1,873,357,991
IOServiceClose call                                 1,873,358,007
Mach episode 2267 begins, message ID 2816           1,873,358,082
wait_queue_assert_wait                              1,873,361,179
SpringBoard thread switches out                     1,873,362,063
```

Before that finalizer, the primary `AppleH1CLCD` object completed open,
framebuffer update, layer-surface lookup, construction, and `new_server`.
Display discovery then opened a second 720x480 object and called its TV-out
setters, identifying it as optional `AppleH1TVOut`. Its shipped selector path
does not assign generic IOMobileFramebuffer's surface-ID field, so surface ID
zero is expected for that optional object; it is not a primary-CLCD failure.
The close did not return, but other guest threads continued to the 2.5 B cap.
This is a SpringBoard-thread wait, not a whole-emulator deadlock.

The bus report and shipped driver close the causal chain for this exact wait:

```text
TV-out control  0x39100000   86 reads / 201 writes
TV-out mixer    0x39200000  105 reads /  45 writes
TV-out SDO      0x39300000   94 reads / 181 writes
```

All three pages were unmapped in run18. VIC0 line 30 was enabled in
`0xc006269f`, but its raw bit never asserted because no device supplied SDO
VSYNC. The close path sleeps while a swap is queued or active; the TV-out IRQ
30 filter/action is the only observed path that clears that work and wakes the
gate. This proves why this close slept. It does not prove that TV-out is the
only remaining boot issue.

The post-run18 implementation maps three independent byte-lane-safe 4 KiB
banks, retains unknown registers, derives ready from each bank's run state,
and generates a 60 Hz level IRQ 30 only when all three run gates are active and
SDO VSYNC is unmasked. SDO `+0x280` bit 0 is latched and write-one-to-clear;
`+0x284` bit 0 masks it. Mixer `+0x4c` is a deliberately nonasserting
write-one-to-clear acknowledgement. It does not fabricate IRQ 38, hotplug, an
IOSurface, framebuffer pixels, or a TV signal. Snapshot v4 persists its banks,
phase, and frame counter, and WFI can advance to the next deliverable IRQ 30.
Focused unit tests pass, but no post-run18 real-firmware trace has validated
the model yet.

For the next firmware run, require this sequence before advancing the claim:

1. the three TV-out pages route through the model rather than the unmapped bus;
2. SDO VSYNC asserts raw VIC0 IRQ 30 under the shipped run/mask state;
3. the shipped IRQ 30 filter/action executes and acknowledges the pending bit;
4. the swap gate wakes and the exact `IOServiceClose` call returns;
5. the target resumes beyond CAWindowServer setup and reaches a later exact
   SpringBoard checkpoint, ideally `applicationDidFinishLaunching:`.

Do not infer a boot or synthesize a return if any link is absent. Run18 also
predates the unified framebuffer planner and stricter CLCD allocation bounds:
the current external-md layout reserves `0x0885c000..0x088f2000`, advances
physical `topOfKernelData` to `0x088f4000`, and validates the page-rounded
`stride * height` mapping through the 4 GiB boundary. A fresh run must
revalidate both hardening changes alongside the TV-out path.

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

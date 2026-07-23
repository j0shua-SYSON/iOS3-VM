<div align="center">

# iOS3-VM

### Project goal: boot **real iPhone OS 3** — Apple's actual kernel, `launchd`, and SpringBoard — inside an app on a modern, jailbroken iPhone.

*A from-scratch, full-system emulator of the 2007 iPhone's silicon, running in a modern pocket.*

[![core-tests](https://github.com/j0shua-SYSON/iOS3-VM/actions/workflows/core-tests.yml/badge.svg)](https://github.com/j0shua-SYSON/iOS3-VM/actions/workflows/core-tests.yml)
[![ios-build](https://github.com/j0shua-SYSON/iOS3-VM/actions/workflows/ios-build.yml/badge.svg)](https://github.com/j0shua-SYSON/iOS3-VM/actions/workflows/ios-build.yml)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![platform](https://img.shields.io/badge/host-iOS%2015%20·%20A9%20·%20jailbroken-black)
![guest](https://img.shields.io/badge/guest-iPhone%20OS%203.1.3%20·%20S5L8900-lightgrey)

</div>

---

## What is this?

> **Target versus current build:** the portable core and the `bootkernel` CLI
> have run Apple's real kernel, root filesystem, `launchd`, and a live
> `mDNSResponder`. A new cold-boot mode exact-gates the 7E18 kernel, device tree,
> and rootfs, then serves a create-only writable work image from the host instead
> of pinning roughly 445 MiB in guest RAM. A guarded cold run first reached raw
> `/dev/rmd0` fsck I/O at **402,741,536** retired instructions after `BSD root:
> md0` and `launchd[1] has started up`. The first fresh 128 MiB real-firmware
> 2 B cold run, run07, extended the completed faultable native-`uiomove64` path
> through its **2,000,000,000**-instruction cap with exit status 0. It retained
> the fsck/root-mount, `mDNSResponder[14]` Seatbelt, and `systemShutdown false`
> evidence, and completed 12,782 external reads plus 82 writes with zero
> failures.
> Both raw reads finished through two native redirects and two checked
> completions, with zero raw guest errors and zero pending continuations. The
> 466,825,216-byte work image did not grow, stderr was empty, and the firmware
> hashes remained unchanged. This serial run explicitly disabled the framebuffer:
> CLCD status, mask, and scanning were all zero, so it provides absolutely no
> SpringBoard or display-path proof.
> Display-enabled run09 then extended the same 128 MiB external-md path through
> its **2,000,000,000**-instruction cap with harness status `OK` and empty
> stderr. It recorded one exact stock SpringBoard `posix_spawn` pathname attempt
> at instruction **635,280,837**. That is materially later lifecycle evidence,
> but the diagnostic does not yet prove that the syscall succeeded, that a
> SpringBoard process ran, or that it rendered. The guest still made no recorded
> CLCD MMIO access; SPI0 saw only 13 early platform writes, and the final frame
> remained the same 8x16 white block at the top-left of an otherwise black
> screen.
> The installable iOS app
> does **not** run it yet: it runs a small
> synthetic ARM guest to exercise the CPU, UART and framebuffer bridge. The app
> currently uses CoreGraphics, with no touch, audio, guest networking or active
> JIT. Moving the bounded CLI path into a shared real-guest session remains the
> next app prerequisite.

iOS3-VM is a **low-level, full-system emulator project**. It doesn't
*reimplement* iPhone OS or fake its apps — it emulates the **hardware** of the
original iPhone's chip (the Samsung **S5L8900** and its ARMv6 core). The long-term
target is for Apple's own boot ROM → iBoot → XNU kernel → `launchd` →
**SpringBoard** chain to run on top of it, believing it is on a real 2009 iPhone.
The current path does not execute SecureROM or iBoot: `bootkernel` synthesizes
the subset of iBoot's handoff needed to enter XNU directly.

The product target puts that whole emulated phone **inside an app on your real
iPhone.**

> **The honest headline:** to the maintainers' knowledge, no publicly documented
> open-source emulator has booted iPhone OS 3.x to a home screen. The closest
> prior art found in the project's survey boots iPhone OS 1.1 and 2.1.1 on
> emulated iPod touch hardware. Treat that as project positioning, not proof that
> no private or unindexed implementation exists.

## Why it's different

The distinctive target is specific rather than categorical: emulate the
S5L8900 and boot the original Apple software stack, while keeping the machine
core portable across hosts. Today the evidence is split deliberately:

| Capability | CLI / portable core | Installable iOS app |
|---|---|---|
| ARM1176 and S5L8900 execution | Real-kernel path recorded | Synthetic demo guest |
| Apple kernel and root filesystem | Host-backed cold path reached `launchd`; run09 passed fsck, mounted `/dev/md0`, retained `mDNSResponder`, and reached a stable 2 B cap | Not integrated |
| Display | Run09 reached one exact SpringBoard spawn-path attempt and PCs in both Apple display-driver code ranges, but syscall success is unproved, CLCD MMIO stayed untouched, and the frame was only one 8x16 white block | CoreGraphics demo bridge |
| Touch, audio, guest networking | Not implemented | Not implemented |
| Dynamic recompiler | Translator tested off-device; inactive in boot | Excluded from target |

## Status

This project is built **milestone by milestone — every stage boots something you
can see.** No months in the dark.

| | Milestone | State |
|---|---|---|
| **M0** | Toolchain online: core builds + tests in CI, iOS `.ipa` builds on a macOS runner, and the app has historically run its ARM self-test on-device | ✅ **done** — current CI proves build/package, not device launch |
| **M1** | ARMv6 (ARM1176) interpreter; unsupported encodings trap | ✅ **done for the reached boot path** — ARM, Thumb, VFPv2, ARM1176 WFI, and the reached ARMv5TE DSP multiply families, with explicit gaps |
| **M2** | S5L8900 bring-up: bare-metal payload prints over emulated UART | ✅ **done** — MMU, bus, UART, VIC, timer, power, CLCD and NOR are integrated; standalone raw-NAND/storage primitives are host-tested, with no NAND controller/VFL/FTL |
| **M3** | Firmware containers + LLB execution | ✅ **done** — parses/decrypts real IMG3 firmware, runs a real LLB payload and extracts the kernel; SecureROM and iBoot execution remain future full-chain work |
| **M4** | The real **XNU kernel** boots and logs | ✅ **done** — a broad set of prelinked drivers matched or started in a recorded CLI run; the real 413 MiB root filesystem mounted, and that run did not reach `_panic` |
| **M5** | `launchd` → **SpringBoard** renders — tap it 🏆 | 🔵 **in progress.** Display-enabled run09 reached a stable 2 B cap with 36.5% USR execution and one exact stock SpringBoard `posix_spawn` pathname attempt at 635,280,837. It does **not** yet prove spawn success, process execution, or rendering: no CLCD MMIO was recorded and the frame remained an 8x16 white block. The iOS app remains a demo host. |

At `ea92fca`, hosted
[`core-tests` run 30009684129](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30009684129)
and
[`ios-build` run 30009684054](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30009684054)
both completed successfully with the faultable raw bridge and corrected display
handoff. Hosted CI cannot contain private firmware or prove a SpringBoard boot;
that runtime evidence comes from the separately recorded run07 through run09
cold boots.

### What it actually does today

In the `bootkernel` CLI harness, Apple's real `xnu-1357.5.30`, decrypted from a
stock 3.1.3 IPSW, ran on hardware that exists only as C in this repo — mounting
a real root filesystem and introducing itself over an emulated Samsung UART:

```
iBoot version:
Seatbelt MACF policy initialized
BSD root: md0, major 2, minor 0
AppleS5L8900XIO::start: chip-revision: EVT0
AppleARMPL192VIC::start: _vicBaseAddress = 0xe3141000
AppleS5L8900XClockController: Dynamic Performance State Management Enabled
AppleARMPL080DMAC::start: dmac0 / dmac1
AppleS5L8900XSDIO::start(): SDIO Revision 8900X
virtual bool AppleMobileFileIntegrity::start(IOService*): built Dec 21 2009
AppleS5L8900XSerial: Identified Serial Port on ARM Device=uart0 at 0x3cc00000
AppleSerialMultiplexer: mux::start: created new mux (18) for spi-baseband
AppleMultitouchZ2SPI: successfully started
IOSDIOController::enumerateSlot(): Searching for SDIO device in slot: 0
IOSDIOController::enumerateSlot(): CMD5 failed ... (no card is modelled)
```

Those messages are produced by **Apple's own kernel extensions**, unmodified,
after matching against the emulated device tree. They are evidence that the
guest reached those drivers, not proof that every matched device is complete:
the emulator models some peripherals, returns bounded placeholder behaviour for
others, and deliberately prevents unsupported accelerators from matching.

### Latest real-guest boundary

The current interpreter has gone substantially beyond the historical VFP stop.
ARM1176 `WFI` now advances emulated devices to the next interrupt that can wake
the CPU without inventing retired instructions, and the full related ARMv5TE
signed-DSP multiply set is implemented: `SMULxy`, `SMLAxy`, `SMLALxy`, `SMULWy`,
and `SMLAWy`.

The latest measured direct-RAM checkpoint chain first restored the real guest at
**2.2 billion**
retired instructions, crossed the former user-mode stop at `0xe1630381`
(`SMULBB r3, r1, r3`), wrote a **2.4-billion** checkpoint, and reached 2.45 B
with `launchd` and `mDNSResponder` alive. A restore at 2.4 B then reached 2.8 B
and wrote a 2.7 B checkpoint. That interval observed one new `_execve`, first at
**2,605,595,575**, and reported `systemShutdown false`. Finally, restoring 2.7 B
wrote a 2.85 B checkpoint and reached the configured **2.9-billion** cap. None of
those continuations reached `_panic`, `Debugger`, or an emulator
undefined-instruction stop. A smaller diagnostic continuation then reached
**2,944,340,624** instructions and stopped fail-closed on `0xe6cf3073`, decoded
as ARMv6 `UXTB16 r3, r3` in user mode. The complete paired signed/unsigned
extend and accumulate family was implemented, with all rotations and illegal
forms tested. Replaying the same 2.85 B checkpoint cleared that instruction,
wrote a **2.97 B checkpoint**, and reached the configured **2.98 B cap** with
status `OK`, no `_panic`, no `Debugger`, and no emulator undefined stop. The
interval recorded two `_load_machfile` paths, 400 code-page validations, 4,266
software-interrupt entries, and 3,373 Unix syscalls.

The same diagnostic clarified the memory risk. Free pages fell from 542
(2.12 MiB; low 539) at 2.8 B and 317 (1.24 MiB; low 301) at 2.9 B to a low of
**97 pages (0.38 MiB) at 2,934,505,472**, recovered to 253 pages by the former
opcode stop, and ended at **214 pages (0.84 MiB)** at 2.98 B, against a 250-page
target. This suggests XNU's reclamation path is active, but it does not make the
layout safe: the roughly 445 MiB pinned RAM disk consumes pages that a real
device would keep on storage. The
host-backed-storage audit ruled out merely relocating md0 or flipping its
physical-mode flag: this kernel's `_bcopy_phys` only understands the normal
DRAM direct map. The portable exact-I/O block API, a descriptor-backed writable
file adapter, a privileged-only fail-closed SVC seam, and the exact-site md
bulk-copy bridge now exist and are host-tested. Mach-O UUID parsing and an
all-or-nothing expected-byte patch transaction underpin an exact 7E18 manifest
that checks the complete decrypted kernel image and parsed layout, fixed RAM
mapping, and all five patch sites before changing guest RAM. The fifth exact
edit replaces `_mdevrw`'s audited four-byte Thumb prologue with
`svc #0xe3; svc #0xe4`. A bounded rootfs provisioner now copies an immutable source into an
unpublished work image, validates the supported HFS+/HFSX layout, rewrites the
unique stock fstab, grows and revalidates the volume, flushes it, and publishes
without replacement. `bootkernel --external-md` now exact-gates the complete
7E18 kernel, device tree, and immutable rootfs before creating that work image;
it publishes a synthetic md0 aperture outside DRAM and installs privileged
strategy and raw-uio bridges. Backend errors halt without falsely retiring the
trapping instruction. The raw bridge validates the exact 32-bit XNU `uio`,
enforces the `0xc0000000` user/kernel split and ARMv6 permissions at 1 KiB
granularity, and stages at most 128 KiB. Resident mappings take the bounded
direct path. A missing user mapping redirects through the exact Thumb
`_uiomove64` entry at `0xc0128d14`, using `ARM_SVC_REDIRECTED` and one of four
128 KiB, kernel-SP-keyed guest bounce slots reserved below `topOfKernelData`;
the second SVC is the checked completion continuation. A zero-initialized,
host-resident 128 KiB tail overlay makes adjacent guard reads and writes
coherent without growing either the immutable source or the work image. This
matches the closest public XNU `_mdevrw`, which performs one `uiomove64` without
a logical EOF bounds check. External mode deliberately rejects snapshots until
backing identity and overlay state are serialized.

Run03 crossed the former raw-entry guard and reached its 420,000,000 cap, but
fsck exited with signal 8. Run04 reproduced the underlying pair during fsck's
`-p` and `-fy` passes: a segment-5 32 KiB read at offset zero faulted while
writing user VA `0x01001000` (`FSR 0x807`), and another 32 KiB read began at
`0x1bd30000` against media end `0x1bd33000`, leaving 12 KiB in media and 20 KiB
in the native md allocation tail. Run05 cleared both cases: two raw reads
completed through two native redirects and completions, with 45,056 media bytes
and 20,480 guard bytes read, zero raw guest errors, and zero pending
continuations. Across strategy and raw paths it completed 6,901 external reads
(28,295,168 bytes), one 512-byte write, and zero failures while preserving the
466,825,216-byte work-image length.

Run06 retained those raw results and extended the cold path to
1,000,000,000 instructions with exit status 0. It printed the launchd/fsck/root
mount sequence, both `mDNSResponder[14]` Seatbelt lines, and the
`systemShutdown false` marker. It completed 10,004 external reads (40,994,304
bytes) and 27 writes
(107,008 bytes), with zero failures; strategy handled 10,002 reads and all 27
writes. Raw I/O remained two reads, two redirects, two completions, zero guest
errors, and zero pending continuations, split into 45,056 media bytes and
20,480 coherent-guard bytes. The low-water sample was 17,221 free pages
(67.27 MiB) at instruction 980,615,168; `_execve` remained at 11 hits while
`_load_machfile` advanced to 25. Stderr was empty, the firmware hashes remained
unchanged, and the work image remained exactly 466,825,216 bytes.

Run07 extended the fresh 128 MiB external-md cold path to
2,000,000,000 instructions with exit status 0. The final PC was `0x3145ad4c` in
USR mode (`CPSR 0x20000010`); 731,259,769 instructions, 36.6% of the run,
retired in USR mode. `_execve` reached 12, `_load_machfile` 32,
`_thread_bootstrap_return` 92,620, and `_unix_syscall` 58,166. The prior
launchd/fsck/`/dev/md0` mount, `mDNSResponder[14]` Seatbelt, and
`systemShutdown false` lines remained present.

The bridge completed 12,782 reads (52,372,992 bytes), 82 writes (325,120
bytes), and zero failures; strategy handled 12,780 reads and all 82 writes.
Raw I/O remained two reads, zero writes, zero guest errors, two native
redirects, two completions, and zero pending continuations. It read 45,056
media bytes plus 20,480 coherent-guard bytes and wrote neither region. The run
ended with 13,000 free pages (50.78 MiB), after a low of 12,983 pages
(50.71 MiB) at instruction 1,836,056,576. Stdout was 234,838 bytes, stderr was
empty, the work image stayed exactly 466,825,216 bytes, and all three source
firmware hashes were unchanged.

The run07 framebuffer was disabled, and CLCD status, mask, and scanning were
all zero. Therefore none of its additional userspace execution is evidence
that SpringBoard started or that the real display path works.

The current CLCD correctness work fixes a separate pre-run prerequisite. The
words at offsets `0x0d8..0x0ec`, previously mislabeled as panel timings, are
per-window auxiliary configuration; the real `VIDTCON0..3` timing registers are
at `0x20c..0x218`. The N82 handoff now seeds the iBoot-compatible 54 MHz
display clock divided by five, inverted-VCLK polarity, and porch/sync state.
Active timing derives from the requested geometry; the production request is
320x480. The initial `0x0d8`, `0x0e0`, and `0x0e8` window words are `0x1000`.
Live scanout is reported only while all three controller gates agree:
start/stop state, the `CLCD_CTRL` global enable, and `VIDCON0` bit 0. A
remembered enabled window by itself is not a running display.

Run08 exercised that corrected seed in a fresh 128 MiB external-md boot with a
framebuffer and CLCD hot-page tracing. The harness reached its 600,000,000 cap
with `stopped ... OK`, at PC `0xc017056c` (`_SHA1Init+0xc4`), and stderr was
empty. The wrapper's exit-marker file was accidentally empty, so this is not a
captured OS process exit status.

Exact PC coverage recorded 675 entries in the `AppleH1DisplayDrivers` bundle
range (first 126,211,220; last 201,032,245) and 409 in `AppleMerlotLCD` (first
209,372,737; last 211,410,011). These are instruction-entry observations, not
proof that each instruction retired or that either driver started. The CLCD
page at `0x38900000` recorded zero accesses. Seeded configuration survived
while guest-time ticking advanced IRQ status and the frame counter: status 1,
mask 0, scanning 1, `CLCD_CTRL = 0x41`, `VIDCON0 = 0x441`, `VIDCON1 = 0x8`,
window 0 active, and 386 frames.

The captured frame was nonblack only technically: 128 white pixels formed one
8x16 block at the top-left and every other pixel was black (384 nonzero RGB
bytes). The lifecycle ring held 70 events with zero pathname-copy failures,
service spawns through `/usr/sbin/notifyd` at instruction 586,776,479, and zero
exact SpringBoard path attempts. User mode retired 44,274,420 instructions
(7.4%), and free pages bottomed at 19,260 (75.23 MiB). The bridge completed
8,059 reads (33,034,752 bytes), 16 writes (61,952 bytes), and zero failures;
its two raw redirects and completions left zero pending requests or guest
errors. The source hashes remained unchanged.

Run08 therefore proves that the CPU reached PCs inside both driver-bundle code
ranges and that seeded scanout survived. It does not prove successful
`AppleH1CLCD` start, SpringBoard, or an Apple-display-driver-driven frame. Zero
MMIO narrows the next investigation but does not by itself identify the blocker.

Run09 extended the corrected display-enabled configuration to a fresh
**2,000,000,000**-instruction cap. The harness reported `stopped ... OK` and
stderr was empty; the wrapper could not provide an OS process exit marker, so
no host exit code is claimed. User mode retired 729,934,906 instructions
(36.5%). Free pages reached a low of 12,976 (50.69 MiB) at instruction
1,829,371,904. The bridge completed 12,798 reads (52,438,528 bytes), 82 writes
(325,120 bytes), and zero failures. The source kernel, device tree, and rootfs
hashes remained unchanged.

The lifecycle ring retained 120 events and observed the exact stock
SpringBoard pathname once, in a `posix_spawn` attempt at instruction
635,280,837. Its single pathname-copy failure was a separate later event. The
attempt is the strongest current SpringBoard-frontier evidence, but entry to
`posix_spawn` alone does not establish its return value, a child process, or a
rendered frame.

Display evidence did not advance with that pathname attempt.
`AppleH1DisplayDrivers` recorded 687 instruction-entry observations (first
126,211,220; last 1,571,737,384); only six late two-instruction callbacks
extended the run08 range. `AppleMerlotLCD` remained at 409 observations, last
at 211,410,011. SPI0 recorded only 13 early platform writes and no panel
transaction, and no CLCD MMIO was recorded. Seeded scanout reached 589 frames,
but the final PPM was byte-identical to run08: exactly 128 white pixels in an
8x16 top-left block, with every other pixel black.

That is sustained real userspace, not a completed boot. There is still no
captured SpringBoard frame, no proof that the current userland reached the home
screen, and no touch, audio, or guest-network path in the app. The CLI evidence
must not be read as an on-device result.

Getting this far needed one more emulator-shaped bug worth naming, because it
looked exactly like a corrupt disk. launchd's first text page was failing its
code-signature hash. The bytes were fine: a private, untracked historical
investigation reported that all 155 signed Mach-Os and 6,731 code pages on the
volume hashed correctly. That verifier is not reproducible from the public
tree. `cs_validate_page` hashes
exactly 4096 bytes, and `SHA1UpdateUsePhysicalAddress` routes exactly-4096-byte
buffers to a *hardware* SHA-1 engine whenever `IOCryptoAcceleratorFamily` has
registered its hook — an engine at 0x38000000 that we do not model, so the digest
came back fabricated. Un-matching that nub keeps the hook NULL and the kernel
hashes in software. The clinching evidence was timing: `SHA1Init` → verdict took
14,329 instructions where software SHA-1 over 4 KB needs ~145,000, so the
software path demonstrably never ran.

Full detail in [`docs/ROADMAP.md`](docs/ROADMAP.md), the boot narrative in
[`docs/BOOTLOG.md`](docs/BOOTLOG.md), and the diagnosis procedure in
[`docs/debugging.md`](docs/debugging.md).

## How it works

```
   Windows dev box                GitHub Actions                 iPhone 6s Plus
  ┌────────────────┐   git push  ┌──────────────────┐  .ipa    ┌──────────────┐
  │ portable C core│ ──────────► │ macOS runner:    │ ───────► │ jailbroken   │
  │ tested locally │             │ xcodebuild →     │          │ iOS 15 · A9  │
  │ (MinGW, no SDK)│             │ ldid fake-sign   │          │ RWX hint only │
  └────────────────┘             └──────────────────┘          └──────────────┘
```

The interpreter, MMU and device core are plain C11 without third-party runtime
dependencies, so they test quickly on ordinary desktop hosts and drop into the
iOS app. Executable-memory allocation is a small platform shim. The current
Apple-specific shell uses UIKit, CoreGraphics and QuartzCore; other hosts should
implement the same host-facing seams without importing Apple APIs into the core. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

**Why an old phone is a promising host:** the A9 is `arm64` but predates Apple's
APRR JIT hardening (A11) and PAC/PPL (A12), so the dynarec is designed around a
plain **RWX** mapping after jailbreak policy permits it. At startup the app only
reports `CS_DEBUGGED` and whether an RWX mapping can be created; it deliberately
does not branch into unsigned generated code, because a policy mismatch could
create a permanent launch crash. An explicit, recoverable device diagnostic is
still needed to prove execution. The guests ran on a 412 MHz single core with
128 MB of RAM; the host is a dual ~1.85 GHz `arm64` with 2 GB.

**Realtime is not promised.** The unmeasured projection in
[`docs/dynarec.md`](docs/dynarec.md) §10.3 places a mature block JIT at roughly
**0.15–0.45x** of the guest model's nominal rate. No A9 throughput result exists
yet, so that range is a planning hypothesis, not a claim about SpringBoard
responsiveness on the phone.

**The dynarec's honest state:** an AArch64 emitter and ARM/Thumb block translator
exist behind `-DIOS3VM_JIT=ON`, and emitted blocks execute in the macOS arm64 CI
tests. There is still no code cache, dispatcher, chaining or invalidation, so the
machine run loop never calls the translator and the iOS app excludes it. It is a
tested foundation, not an active boot engine. [`docs/dynarec.md`](docs/dynarec.md)
§0 keeps the score.

## Build & run

The hosted app build needs no local Apple SDK or toolchain — CI performs the
Apple-specific build. Booting Apple software still requires firmware supplied by
the user.

**Test the core locally** (any OS with a C compiler + CMake):
```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The public suite runs without Apple firmware; additional symbol/kext checks are
enabled only when the user supplies a kernelcache. Suite and assertion counts
change as coverage grows, so use the current test output and CI logs rather than
a number copied into this README. The build defaults to `Release` — the
interpreter is the hot loop of everything here, and optimization has measured a
substantial speedup in historical boot runs.

**Optionally build the dynarec** (off by default; see the status above and
[`docs/dynarec.md`](docs/dynarec.md)):
```sh
cmake -S . -B build-jit -DCMAKE_BUILD_TYPE=Release -DIOS3VM_JIT=ON
cmake --build build-jit --config Release
ctest --test-dir build-jit -C Release --output-on-failure
```

**Boot the kernel** once you have supplied firmware (see below). The recommended
cold path keeps the immutable source unchanged and requires a work path that
does not already exist:
```sh
mkdir -p work
build/core/bootkernel firmware/kernel.macho \
    -d firmware/devicetree.bin \
    -c "debug=0x8 serial=1 nand-enable-adm=0" \
    --external-md firmware/rootfs.img work/rootfs-7e18-run01.img \
    -R 128 \
    -n 420000000
```

External-md mode accepts only the measured 7E18 kernel, 40,544-byte device tree,
and 433,274,880-byte rootfs identities. It creates, flushes, and publishes the
grown work image without replacement, then opens that image through a bounded
host block adapter. With the default growth, that file is exactly 466,825,216
bytes (445.199 MiB); budget at least 500 MiB plus log and filesystem headroom on
the work volume. The parent directory must exist. It is cold-boot only. A raw
guest errno, storage failure, undefined instruction, guest `_panic`/`_Debugger`
entry, or other guest halt exits nonzero. The published work image is
deliberately preserved after such a failure, and the next invocation
refuses to reuse its path: archive or remove it deliberately, or choose a new
filename. Larger `--grow` values can take the file up to the 512 MiB volume
ceiling. A cleanup warning means a second large temporary may also remain in the
work directory and must be inspected before retrying.

The source kernel, device tree, and rootfs remain byte-for-byte unchanged. Five
firmware-specific patches are applied only to the loaded kernel copy, iBoot-style
properties are edited only in the in-memory device-tree copy, and fstab/growth
changes are made only in the separate work image.

| Accepted input | Bytes | SHA-256 |
|---|---:|---|
| `firmware/kernel.macho` | 7,942,144 | `0d8cdb339d37cf37a1db2638fff79272ecd63a17764bf7666efa1618725df70c` |
| `firmware/devicetree.bin` | 40,544 | `4867c95fedf544bda2ecaa2626ae14c01a60d7771dc53ffe6fd3a6aac8b8ba57` |
| `firmware/rootfs.img` | 433,274,880 | `c3251e7f092c939d5818e92086cb47680981cfb03731de7b55d238c942eb5e82` |

The historical direct-RAM mode remains available with
`-r firmware/rootfs.img -R 512` for checkpoint replay. In that mode, `-R 512`
is load-bearing rather than a preference: `arm_vm_init` hardcodes
`virtual_avail = 0xe0000000`, so advertising more than 512 MiB of guest RAM makes
the kernel's zone bootstrap fault at the documented virtual base. The current
machine also rejects a larger aperture because SDRAM is exactly
`[0x08000000, 0x28000000)` and NOR starts at `0x28000000`. Historical 768 MiB
experiments are therefore not valid current recipes. `nand-enable-adm=0` keeps
`AppleS5L8900XADMFMC` from panicking on a NAND controller we do not model. Three
further workarounds are applied automatically and printed in the run header: the
IORTC wait patch, the MBX GPU-node unmatch, and the SHA-1 accelerator-node
unmatch. `-g` and `-S` deliberately re-enable the two known-broken hardware
paths for diagnostics; external-md rejects `-K`, which would disable its exact
kernel patch set.

Both large-input paths are host-memory bounded. External-md provisioning hashes
the exact bytes copied through a buffer of at most 1 MiB and performs every HFS
edit on an unpublished temporary file. Direct-RAM mode sizes and validates the
guest layout first, allocates guest DRAM once, and streams the rootfs into its
final range through the same retained source handle.

Two filesystem transformations are applied to the writable work image (or to the
**loaded copy** in
legacy `-r` mode); `firmware/rootfs.img` itself is never written. The guest's
`/private/etc/fstab` is retargeted at `md0`, because the stock record names a
`disk0` that only exists behind the undocumented NAND stack. Legacy direct-RAM
mode alone accepts `--keep-fstab` to reproduce launchd's halt; external-md
rejects it. The volume is **grown** by `--grow` MB, default 32: Apple sizes the
system dmg exactly to its contents — `freeBlocks == 0` —
because on hardware everything writable lives on `disk0s2`, which this machine
does not have, so without this launchd and the daemons cannot create a single
file. In external-md mode growth enlarges only the host work file. In legacy
`-r` mode it comes out of the guest's free page pool 1:1; `-Y` recovered that
space only in historical 768 MiB experiments that current source rejects for
overlapping NOR. At the valid 512 MiB physical ceiling it is not a usable
headroom recipe. `docs/BOOTLOG.md` has the numbers and the TN1150 detail.

### The tools

| | |
|---|---|
| `bootkernel` | boots the kernelcache and reports where it stopped: milestone probes, a sampled profile, every non-RAM page touched with the PC that touched it, abort sites, and the guest's console output |
| `bootkernel --external-md <source> <new-work>` | exact-gate the supported 7E18 firmware set, create a writable rootfs work image, and cold-boot md0 through guarded strategy and raw-uio host bridges without reserving the disk in guest DRAM; snapshots remain rejected |
| `bootkernel -L` | print the prelinked kext load map and exit without booting |
| `bootkernel --snapshot-at <insn> <file>` / `--restore <file>` | save and resume the currently modelled machine. Instruction positions and diagnostics are 64-bit and absolute across restore; unreachable, missed, malformed and incompatible checkpoint requests fail nonzero instead of silently omitting output. The current chain restored at 2.2, 2.4, 2.7 and 2.85 B, wrote checkpoints at 2.4, 2.7, 2.85 and 2.97 B, and reached a clean 2.98 B cap after clearing the `UXTB16` stop; older timing numbers are host/commit-specific |
| `snapboot` | the snapshot acceptance harness — also prints a machine-derived report, because comparing two snapshot files alone lets a field the format never stores cancel out on both sides |
| `machoinfo <kernel> -k` / `-r <addr>` | dump the kext load map, or resolve one address to a kernel symbol or `<bundle-id>+0xNNNN` |
| `img3dump`, `unlzss`, `runfw` | firmware container, compression, and bare-payload tools |

`docs/debugging.md` is the procedure these add up to.

**Get the app:** on a matching-path push or a manual dispatch, the `ios-build`
workflow produces an `ldid` ad-hoc/fake-signed `iOS3VM.ipa` and uploads it as a
temporary GitHub Actions artifact, subject to the repository's artifact-retention
policy. CI builds, signs and packages the candidate; it does not install or
launch it. Installation still requires a method compatible with the device's
jailbreak, such as an appropriate AppSync or TrollStore setup. No Apple
Developer account is used by the workflow.

**Boot an OS in the CLI harness** (from M3 on): place your **own** iPhone OS
3.1.3 IPSW-derived files and documented decryption keys in the repository's
git-ignored `firmware/` directory — see [`docs/BOOT_CHAIN.md`](docs/BOOT_CHAIN.md).
Reaching the root mount additionally needs the IPSW's root DMG decrypted with
the published RootFS key; `--external-md` uses it as an immutable source, while
legacy `-r` loads it into guest DRAM. The IPA has no firmware importer or
real-guest session yet, so these
instructions do not currently make the app boot the OS. No Apple firmware is
committed or bundled.

## Requirements

- **First validation host:** a **jailbroken iPhone 6s Plus** (Apple A9) on iOS
  15. Installation needs a compatible signing/jailbreak environment. The JIT is
  not active; a separate opt-in diagnostic must confirm emitted-code execution
  on the actual device before that path is enabled. Startup's `CS_DEBUGGED` and
  RWX-mapping report is only a preflight hint.
- **Firmware:** your own iPhone OS 3.1.3 image + keys. **No Apple firmware image
  or decryption key is bundled.**

## Legal

iOS3-VM is an independently written emulator under the MIT license. It ships
**no Apple firmware images or decryption keys.** You supply firmware you are
entitled to use.
"iPhone", "iOS", and "iPhone OS" are trademarks of Apple Inc.; this project is
not affiliated with or endorsed by Apple.

## Credits

Created by [**j0shua-SYSON**](https://github.com/j0shua-SYSON).

Standing on the shoulders of the reverse-engineering community whose public
research made the S5L8900 boot chain and iPhone OS 3 firmware keys knowable.

<div align="center">

**If a real 2009 iPhone booting inside a 2015 iPhone sounds worth watching —
give it a ⭐ and follow along, milestone by milestone.**

</div>

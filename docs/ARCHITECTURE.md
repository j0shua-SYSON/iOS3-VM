# Architecture

iOS3-VM is a **full-system, low-level emulator project**. It does not fake or
reimplement iPhone OS — it emulates the *hardware* (the Samsung **S5L8900** SoC
and its ARM1176JZF-S core). The long-term target is for Apple's boot ROM, iBoot,
XNU kernel, `launchd`, and SpringBoard to run on top of it, believing they are on
a real 2009-era iPhone. Today `bootkernel` bypasses SecureROM and iBoot and
synthesizes the handoff into XNU; executing the full chain remains future work.

That target — a genuine Apple OS booting on emulated silicon — is why the design
looks the way it does.

```
┌──────────────────────────────────────────────────────────────┐
│  iOS 15 app (Objective-C, arm64)          app/                │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ UIKit · CoreGraphics demo framebuffer · no input/audio  │  │
│  └───────────────▲───────────────────────┬────────────────┘  │
│                  │ copied frame            │ future host I/O   │
│  ┌───────────────┴───────────────────────▼────────────────┐  │
│  │           EMULATOR CORE (portable C11)     core/         │  │
│  │                                                          │  │
│  │   ARM1176 CPU  ──► interpreter ───────► guest execution  │  │
│  │   (ARMv6)       JIT translator (built/tested, inactive)  │  │
│  │        │                                                 │  │
│  │        ▼              system bus / MMU                   │  │
│  │   ┌──────────────────────────────────────────────────┐  │  │
│  │   │ Integrated: VICs · timer · power · UART0 · CLCD ·  │  │  │
│  │   │ NOR. Raw NAND substrate is standalone, not wired.  │  │  │
│  │   └──────────────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
        ▲                                    ▲
   user-supplied                        runs on the
   iPhone OS 3 IPSW + keys              jailbroken A9 host
```

## Why the core is plain C

The single most important structural decision: **the interpreter, MMU and device
core are portable C11 without third-party runtime dependencies.** They compile
on Windows, Linux and macOS and drop into the iOS app. Executable-memory
allocation and cache policy live behind a small platform shim.

This buys us three things that matter enormously for a project this hard:

1. **Short test loops off-device.** The ARM core is unit-tested on the
   development machine (see `core/tests/`) — no phone, no cloud, no Xcode. A CPU
   bug can be caught by `ctest` before a slower device round-trip; current test
   and CI logs, rather than a fixed time claim, are authoritative.
2. **A dependency-free build.** No glib, no pixman, no meson — the things that
   make porting QEMU to iOS a multi-month ordeal. The core uses a small CMake
   workflow in CI.
3. **The interpreter is the dynarec's oracle.** The JIT has separate emitted
   ALU and flag semantics, sharing helpers only where practical. Differential
   focused execution tests run short blocks through both engines and compare
   their final architectural state and touched memory. A full per-instruction
   boot differential harness remains planned.

The current iOS-specific code — UIKit, CoreGraphics, lifecycle and an executable-
memory preflight report — lives in `app/` and is the *only* part that needs
Xcode. Touch, audio and networking adapters are targets, not current components.

## Execution engines

The interpreter is the only backend used by the machine run loop today. A
second backend is under construction:

- **Interpreter** (`core/src/arm/arm_interp.c`) — the portable reference backend;
  unsupported forms trap. This is what boots today and what the dynarec is
  validated against. The reached path includes VFPv2 and the ARMv5TE
  `SMULxy`/`SMLAxy`/`SMLALxy`/`SMULWy`/`SMLAWy` families. Milestone M1.
- **Dynarec / JIT** — an AArch64 emitter and ARM/Thumb block translator exist
  behind `IOS3VM_JIT`, and emitted blocks run in macOS arm64 CI. There is no code
  cache or dispatcher, nothing in `s5l8900_run()` calls it, and the iOS target
  excludes it. It is a parallel milestone, not a current execution engine.

## Why the iPhone 6s Plus (Apple A9) is the first host target

Counter-intuitively, an old phone is the better JIT target:

- The A9 is **arm64 (ARMv8-A), not arm64e.** It predates the later APRR/PAC/PPL
  mechanisms, making a plain simultaneous-RWX mapping a plausible and unusually
  simple JIT policy. That is still a hypothesis about the actual jailbroken
  process policy: a separate opt-in `mmap`/emit/call diagnostic must succeed on
  the target before the dynarec can rely on it. The launch path must never make
  this potentially fatal branch.
- A compatible jailbreak may mark the process `CS_DEBUGGED`. That flag is a
  relevant policy signal, not proof that unsigned code can execute under a
  particular jailbreak. The current app observes it with `csops`
  and probes whether an RWX mapping can be created, but it never executes that
  mapping during startup. The app embeds `get-task-allow` and
  `dynamic-codesigning` requests, but it does not invoke a jailbreak service or
  record a successful target-device execution result. Any service integration
  belongs in an optional host adapter, not in the portable core.
- 2 GB RAM is tight for holding a guest OS image. The app requests
  `com.apple.developer.kernel.increased-memory-limit`, but that is not a memory
  guarantee on every device. The real-guest host must measure the live budget,
  preserve the CLI's direct-to-guest rootfs streaming, and fail cleanly under
  pressure. A current CLI continuation dipped to 97 free guest pages
  (0.38 MiB), recovered to 253 near 2.944 B and ended at 214 against a 250-page
  target at the clean 2.98 B cap. The movement is encouraging, but
  such headroom is still a correctness/availability constraint, not future tuning.

The current S5L8900/iPhone 3G target had **128 MB** of RAM and an approximately
**412 MHz** single core. A later S5L8920/iPhone 3GS target would instead be in the
256 MB / 600 MHz class. The iPhone 6s Plus host has a dual ~1.85 GHz arm64 CPU and is the first
optimization and on-device validation target, not an excuse to couple emulator
semantics to UIKit, Darwin sockets or one jailbreak. Other hosts keep the
interpreter and device models and supply their own presentation, storage, audio,
network and JIT-memory adapters.

## Current host boundary and next prerequisite

`tools/bootkernel` owns the real-firmware boot orchestration today. The iOS app
owns a separate synthetic guest and a fixed 320×480 publication buffer. Before
touch, audio or networking can form an on-device vertical slice, that
orchestration must become a shared, portable guest-session API with bounded
scratch memory and random-access firmware input. The portable `vm_source`
boundary now provides exact ranged reads, short-read/retry handling and
cancellation. Legacy checkpoint replay streams the RAM disk directly into final
guest memory, but the shared app session should adopt the guarded external block
backend and fixed 128 MiB geometry instead of pinning a roughly 445 MiB disk in
guest RAM.

Direct streaming fixes host duplication, but it does not fix the guest physical
layout: the entire grown RAM disk remains pinned below `topOfKernelData`. Free
pages fell to 542 at 2.8 B and 317 at 2.9 B, then reached a low of 97 before
recovering to 253 and ending at 214 at the clean 2.98 B cap; the guest target
is 250. The movement suggests guest reclamation is active, but the
headroom remains unsafe for an iOS host. The selected external storage bridge is
integrated without baking a desktop file API into the core. Its first 128 MiB
real cold-boot trace reached `launchd` at a 400 M cap with 21,826 free pages
(85.26 MiB), after 6,695 successful bridged reads and no storage failure. The
write exit was not reached in that bounded run. The old `UXTB16` stop is now
replay-cleared by a complete paired-extend implementation.

The audited near-term storage design retains the proven md0/HFS path but moves
its writable bytes behind a portable block backend. The core now has exact
64-bit ranged reads/writes, bounded host callback sizes, flush, cancellation,
and explicit identity/generation metadata. The host adapter uses retained file
descriptors, denies conflicting opens where the platform permits it, and
revalidates file size and identity around I/O. Its ARM bus also has a
privileged-only SVC seam: handled calls retire normally, ordinary calls remain
guest SVCs, and backend errors restore CPU state and halt without incrementing
the retired-instruction counter. The exact-site md strategy bridge stages reads
before publishing them to RAM, bounds each operation to one 4 KiB page, and
preserves the audited register file. A separate raw bridge owns a bounded
128 KiB scratch buffer and transfer plan; it validates the exact 32-bit XNU
`uio`/8-byte iovec layout, user-address ceiling, MMU permissions, media range,
and metadata aliasing before backend I/O. LC_UUID parsing and an atomic
expected-byte patch manifest gate the firmware-specific edits. The concrete
7E18 manifest also checks the complete decrypted image and parsed Mach-O
layout, fixed mapping, expected bytes at all five sites, and loaded zero-fill
tails before changing RAM. `bootkernel --external-md` wires both bridges into a
fixed 128 MiB cold-boot configuration. It exact-gates the original device tree
and rootfs too, provisions a create-only work image, publishes a synthetic media
token outside DRAM, and installs the bridge multiplexer only after setup
succeeds.

The boot integration uses five exact, kernel-identity-gated 7E18 patches: one
removes the unmodelled IORTC wait, one selects md physical mode, and two replace
only md strategy's `_bcopy_phys` calls with privileged, range-gated bulk-copy
exits. The fifth replaces `_mdevrw`'s audited four-byte Thumb prologue with
`svc #0xe3; svc #0xe4`. The raw handler returns `ARM_SVC_REDIRECTED`: a direct
result redirects to the saved caller LR, while a faultable request redirects to
the exact Thumb `_uiomove64` entry at `0xc0128d14` and returns through the
second, completion SVC. A plain external
physical aperture is not sufficient because this kernel's `_bcopy_phys`
converts both operands through the fixed DRAM direct-map delta and calls
ordinary `_bcopy`; it never reaches the emulator bus. The host-side
provisioner now creates a bounded full work image from an immutable source,
validates the narrowly supported HFS+/HFSX layout, performs the fstab/growth
transaction on an unpublished temporary, flushes it and publishes without
replacement. The generic provisioner does not authenticate firmware, so
`bootkernel` exact-gates the known 7E18 rootfs before calling it and additionally
authenticates the kernel and device tree. Snapshot coupling to backing identity
and overlay generation remains future work after the first cold-boot slice.
Backend failure pauses the VM visibly; it must never be converted to zero-filled
successful I/O.
`/dev/rmd0` is a separate raw-character path through `_uiomove64`/`_copypv`, not
one of the two strategy calls. The raw bridge reproduces the reached
`mdevrw`/`uio_update` contract, including physical-user segment variants,
partial final iovecs, the exclusive `0xc0000000` user limit, and legacy ARM
1 KiB permission subpages. Resident user mappings use the bounded host-direct
path. A missing translation instead allocates one of four 128 KiB guest-RAM
bounce slots keyed by the entry kernel SP, temporarily selects the corresponding
physical-user segment, and lets XNU's own `_uiomove64` perform demand-zero and
COW fault handling. The completion SVC validates the pending SP and resulting
`uio`, restores the original segment, persists any completed write prefix, and
redirects to the saved LR. The slots are page-aligned, pairwise disjoint, and
reserved below `topOfKernelData`, so the guest allocator cannot reuse them.
Malformed or out-of-aperture bounded requests still return guest `ENXIO`,
`EINVAL`, or `EFAULT`; a host backend failure remains a fail-closed VM halt.

The closest public XNU `_mdevrw` performs one `uiomove64` and does not enforce a
logical EOF bound. The compatibility aperture therefore extends only 128 KiB
beyond the work image. A zero-initialized host-memory overlay makes tail writes
visible to later reads while the source and work files retain their exact
sizes. Anything beyond that bounded tail fails closed. This is narrower than
silently extending the host file while preserving allocation-tail coherence.

Run03 proved that the old raw-entry guard can be crossed, reaching its
420,000,000-instruction cap with `launchd` and fsck active, but fsck exited with
signal 8. Run04 at 405 M isolated the two repeatable causes: a write-side
demand-page fault at user VA `0x01001000` (`FSR 0x807`) on the 32 KiB offset-zero
raw read, and a 32 KiB read at `0x1bd30000` crossing media end `0x1bd33000`
(12 KiB media plus 20 KiB allocation tail). The `-p` and `-fy` fsck passes
reproduced the pair.

Fresh run05 provides the real-firmware evidence for the redirected design. It
reached its 430,000,000-instruction cap with exit status 0 after `launchd`,
`Running fsck on the boot volume...`, and
`/dev/md0 on / (hfs, local, noatime)`. Both raw
reads completed through native `_uiomove64`: two redirects, two completions,
zero pending continuations, 45,056 media bytes, and 20,480 coherent-tail bytes.
The complete external path recorded 6,901 reads (28,295,168 bytes), one
512-byte write, and zero failures; the work image remained exactly 466,825,216
bytes. The lowest free-page observation was 20,820 pages (81.33 MiB) at
425,852,928 instructions.

Run06 extended the same architecture to a clean 1,000,000,000-instruction cap.
It retained the two-redirect/two-completion raw result with zero guest errors
and zero pending continuations, while the aggregate external path reached
10,004 reads (40,994,304 bytes), 27 writes (107,008 bytes), and zero failures.
Strategy handled 10,002 reads and all 27 writes; the raw split remained 45,056
media bytes plus 20,480 coherent-tail bytes. The low-water mark was 17,221
pages (67.27 MiB) at 980,615,168 instructions. The work image stayed exactly
466,825,216 bytes, stderr was empty, and the source firmware hashes were
unchanged.

Run07 extended the same 128 MiB external-md architecture to a clean
2,000,000,000-instruction cap. It ended at PC `0x3145ad4c` in USR mode
(`CPSR 0x20000010`), after 731,259,769 USR instructions (36.6%).
`_thread_bootstrap_return` reached 92,620 and `_unix_syscall` 58,166. The
external path completed 12,782 reads (52,372,992 bytes), 82 writes (325,120
bytes), and zero failures; strategy accounted for 12,780 reads and all writes.
The raw two-redirect/two-completion result remained unchanged, with no raw
writes, guest errors, pending continuations, or guard writes. Final free memory
was 13,000 pages (50.78 MiB); the low was 12,983 pages (50.71 MiB) at
1,836,056,576 instructions. The work image and source hashes remained exact,
and stderr was empty.

This validates the compatibility seam at the reached path, but it does not
prove that SpringBoard rendered. Run07 disabled the framebuffer, and its CLCD
status, mask, and scanning values were all zero; it provides no display-path
evidence. A future IOMedia device or full NAND/VFL/FTL model can replace this
compatibility seam without changing the block/session API.

In the planned shared-session design, host services cross explicit non-blocking
seams: frame descriptors out; bounded touch, PCM and network queues in/out;
storage and monotonic-clock operations; and a platform-specific JIT allocator.
The CPU thread remains the sole owner of machine state. UIKit, AVFAudio,
`kqueue` and similar APIs stay in host adapters.

## Memory map and idle-time invariants

The historical direct-RAM configuration models 512 MiB of SDRAM at
`[0x08000000, 0x28000000)`. NOR begins at `0x28000000`, so those half-open ranges
meet exactly and do not overlap. External-md instead fixes real-device-sized
128 MiB DRAM at `[0x08000000, 0x10000000)` and keeps its media token in
`[0xe0000000, 0x100000000)`, outside guest RAM. Machine initialization rejects a
larger direct aperture; older `-R 768` measurements are retained only as history.
The loader proves the active kernel, device tree, boot arguments, optional
direct RAM disk, external-md bounce reservation, and framebuffer ranges are
contained and pairwise disjoint before execution. Firmware-specific edits are
made only to the loaded kernel and device-tree copies; the authenticated source
files remain original and immutable.

ARM1176 idle is modelled as a device-time operation rather than a fake CPU loop.
The exact CP15 wait-for-interrupt form used by XNU invokes a machine callback
that advances the timer and CLCD to the earliest enabled VIC edge capable of
waking the core. It does not increment the retired-instruction counter while the
CPU waits, and falls back safely when no future wake event is known. This keeps
snapshot instruction indices meaningful while avoiding billions of artificial
idle iterations.

## Guest hardware target: S5L8900

| | |
|---|---|
| SoC | Samsung **S5L8900** |
| CPU | ARM1176JZF-S, **ARMv6** (ARM + Thumb) @ 412 MHz |
| Used by | original iPhone, iPhone 3G, iPod touch 1G |
| Display | 320×480 |
| Target OS | **iPhone OS 3.1.3** (last of the 3.x line; decryption keys are public) |

The S5L8900 is deliberately chosen: it is the application processor of the
**iPhone 2G/3G** (minus the baseband/telephony block), so reaching it is the
shortest path to something that boots a *real iPhone's* OS. ARMv6 is also
simpler to emulate correctly than the ARMv7 (Cortex-A8, S5L8920) of the 3GS.

See [BOOT_CHAIN.md](BOOT_CHAIN.md) for how the firmware actually boots, and
[ROADMAP.md](ROADMAP.md) for the milestone plan.

## Build & deploy pipeline

```
  Development host (workspace-local builds and caches)
        │  write C + Obj-C, test core locally with MinGW/CMake
        ▼
  git push  ──►  GitHub Actions
        ├─ core-tests.yml   ubuntu + macOS: cmake + ctest   (seconds)
        └─ ios-build.yml    macOS runner: xcodebuild → ldid fake-sign → .ipa
        ▼
  download .ipa  ──►  install demo app on jailbroken iPhone 6s Plus
```

No macOS, Xcode, or WSL is needed on the development machine — a hosted macOS
runner performs the Apple-specific build. The workflow **fake-signs** the app
with `ldid -S` and does not use an Apple Developer account; installation still
depends on a compatible jailbreak/signing setup on the device.

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
cancellation, while `bootkernel` already sizes the layout and streams the RAM
disk directly into final guest memory. The shared app session must adopt those
pieces rather than duplicating a roughly 445 MiB image beside 512 MiB of guest
RAM.

Direct streaming fixes host duplication, but it does not fix the guest physical
layout: the entire grown RAM disk remains pinned below `topOfKernelData`. Free
pages fell to 542 at 2.8 B and 317 at 2.9 B, then reached a low of 97 before
recovering to 253 and ending at 214 at the clean 2.98 B cap; the guest target
is 250. The movement suggests guest reclamation is active, but the
headroom remains unsafe for an iOS host. The selected storage bridge must recover
guest memory without baking a desktop file API into the core. The old `UXTB16` stop
is now replay-cleared by a complete paired-extend implementation.

The audited near-term storage design retains the proven md0/HFS path but moves
its writable bytes behind a portable block backend. Three exact, hash-gated
7E18 guest patches select md physical mode and replace only md strategy's two
`_bcopy_phys` calls with privileged, range-gated bulk-copy exits. A plain
external physical aperture is not sufficient because this kernel's
`_bcopy_phys` converts both operands through the fixed DRAM direct-map delta and
calls ordinary `_bcopy`; it never reaches the emulator bus. The portable layer
therefore needs exact 64-bit ranged reads/writes, flush, cancellation, bounded
cache, immutable source plus generational COW, and snapshot coupling to backing
identity and overlay generation. Backend failure pauses the VM visibly; it must
never be converted to zero-filled successful I/O. A future IOMedia device or
full NAND/VFL/FTL model can replace this compatibility seam without changing
the block/session API.

In the planned shared-session design, host services cross explicit non-blocking
seams: frame descriptors out; bounded touch, PCM and network queues in/out;
storage and monotonic-clock operations; and a platform-specific JIT allocator.
The CPU thread remains the sole owner of machine state. UIKit, AVFAudio,
`kqueue` and similar APIs stay in host adapters.

## Memory map and idle-time invariants

The current enlarged boot configuration models 512 MiB of SDRAM at
`[0x08000000, 0x28000000)`. NOR begins at `0x28000000`, so those half-open ranges
meet exactly and do not overlap. Machine initialization rejects a larger RAM
aperture; older `-R 768` measurements are retained only as history. The loader
also proves the kernel, device tree, boot arguments, RAM disk and optional
framebuffer ranges are contained and pairwise disjoint before streaming a byte
of the rootfs.

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

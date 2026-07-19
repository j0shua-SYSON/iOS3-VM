# Architecture

iOS3-VM is a **full-system, low-level emulator**. It does not fake or reimplement
iPhone OS — it emulates the *hardware* (the Samsung **S5L8900** SoC and its
ARM1176JZF-S core) faithfully enough that Apple's own **unmodified** boot ROM,
iBoot, XNU kernel, `launchd`, and SpringBoard run on top of it, believing they
are on a real 2009-era iPhone.

That target — a genuine Apple OS booting on emulated silicon — is why the design
looks the way it does.

```
┌──────────────────────────────────────────────────────────────┐
│  iOS 15 app (Objective-C, arm64)          app/                │
│  ┌────────────────────────────────────────────────────────┐  │
│  │  UIKit shell · Metal framebuffer view · touch input     │  │
│  └───────────────▲───────────────────────┬────────────────┘  │
│                  │ framebuffer/vsync      │ multitouch/keys   │
│  ┌───────────────┴───────────────────────▼────────────────┐  │
│  │           EMULATOR CORE (portable C11)     core/         │  │
│  │                                                          │  │
│  │   ARM1176 CPU  ──►  interpreter  ─┐                      │  │
│  │   (ARMv6)           dynarec (JIT) ─┴─► guest execution   │  │
│  │        │                                                 │  │
│  │        ▼         system bus / MMU / TLB                  │  │
│  │   ┌──────────────────────────────────────────────────┐  │  │
│  │   │ S5L8900 devices: VIC · timers · GPIO · UART ·     │  │  │
│  │   │ NAND+FTL · NOR · LCD · multitouch · AES/SHA/PKE · │  │  │
│  │   │ SPI · I2C · clock/PMU                             │  │  │
│  │   └──────────────────────────────────────────────────┘  │  │
│  └──────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘
        ▲                                    ▲
   user-supplied                        runs on the
   iPhone OS 3 IPSW + keys              jailbroken A9 host
```

## Why the core is plain C

The single most important structural decision: **the emulator core is portable
C11 with zero platform dependencies.** Every byte of CPU, MMU, and device logic
compiles and runs on Windows, Linux, and macOS *and* drops unchanged into the
iOS app.

This buys us three things that matter enormously for a project this hard:

1. **Sub-second test loops off-device.** The ARM core is unit-tested on the
   development machine (see `core/tests/`) — no phone, no cloud, no Xcode. A CPU
   bug is caught in the time it takes to run `ctest`, not after a 10-minute
   device round-trip.
2. **A dependency-free build.** No glib, no pixman, no meson — the things that
   make porting QEMU to iOS a multi-month ordeal. Our CI for the core is three
   lines of CMake.
3. **The interpreter is the dynarec's oracle.** Because both share the exact
   same C semantics module, the JIT can be differentially tested against the
   interpreter instruction-by-instruction.

The iOS-specific code — UIKit, Metal, touch, and the JIT memory dance — lives in
`app/` and is the *only* part that needs Xcode.

## The two execution engines

The CPU has two interchangeable backends behind one interface (`arm_step`):

- **Interpreter** (`core/src/arm/arm_interp.c`) — correct, portable, simple.
  This is what boots first and what the dynarec is validated against. Milestone
  M1.
- **Dynarec / JIT** — translates blocks of guest ARMv6 into host **ARM64**
  emitted into plain **RWX** pages, for near-realtime speed. Milestone (parallel
  track). See below for why the host device makes this unusually clean.

## Why the iPhone 6s Plus (Apple A9) is the *right* host

Counter-intuitively, an old phone is the better JIT target:

- The A9 is **arm64 (ARMv8-A), not arm64e.** It predates **APRR** (Apple's
  JIT-hardening permission mechanism, introduced with the A11) and **PAC** +
  **PPL** (introduced with the A12) — the A9 has none of them. So there is no
  per-thread hardware W^X enforcement, and once code-signing is relaxed we get
  true, simultaneous **RWX** pages from a plain
  `mmap(PROT_READ|PROT_WRITE|PROT_EXEC)`. No `MAP_JIT`, no
  `pthread_jit_write_protect_np()` toggling, no double-mapping tricks.
- The device is **jailbroken** (Dopamine 2.x, or palera1n). The jailbreak
  kernel-patches our process to `CS_DEBUGGED`, which is what makes unsigned
  executable memory legal. We detect this at launch (`csops`, `CS_DEBUGGED`)
  and can request it explicitly via `libjailbreak`.
- 2 GB RAM is tight for holding a guest OS image, so the app carries the
  `com.apple.developer.kernel.increased-memory-limit` entitlement to lift the
  default jetsam per-app ceiling.

The guests we target had **128–256 MB** of RAM and a **412–600 MHz** single
core, versus the host's dual ~1.85 GHz arm64 — so with a working dynarec,
realtime is a reasonable goal, not a fantasy.

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
  Windows dev box (C: full → everything on F:)
        │  write C + Obj-C, test core locally with MinGW/CMake
        ▼
  git push  ──►  GitHub Actions
        ├─ core-tests.yml   ubuntu + macOS: cmake + ctest   (seconds)
        └─ ios-build.yml    macOS runner: xcodebuild → ldid fake-sign → .ipa
        ▼
  download .ipa  ──►  install on jailbroken iPhone 6s Plus
```

No macOS, Xcode, or WSL is needed on the development machine — the macOS runner
(free and unlimited on public repos) does the Apple-specific build. Because the
device is jailbroken, the app is **fake-signed** (`ldid -S`) and needs no Apple
Developer account.

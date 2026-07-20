<div align="center">

# iOS3-VM

### Boot **real iPhone OS 3** — Apple's actual kernel, `launchd`, and SpringBoard — inside an app on a modern, jailbroken iPhone.

*A from-scratch, full-system emulator of the 2007 iPhone's silicon, running in 2025's pocket.*

[![core-tests](https://github.com/j0shua-SYSON/iOS3-VM/actions/workflows/core-tests.yml/badge.svg)](https://github.com/j0shua-SYSON/iOS3-VM/actions/workflows/core-tests.yml)
[![ios-build](https://github.com/j0shua-SYSON/iOS3-VM/actions/workflows/ios-build.yml/badge.svg)](https://github.com/j0shua-SYSON/iOS3-VM/actions/workflows/ios-build.yml)
[![license](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![platform](https://img.shields.io/badge/host-iOS%2015%20·%20A9%20·%20jailbroken-black)
![guest](https://img.shields.io/badge/guest-iPhone%20OS%203.1.3%20·%20S5L8900-lightgrey)

</div>

---

## What is this?

iOS3-VM is a **low-level, full-system emulator**. It doesn't *reimplement* iPhone
OS or fake its apps — it emulates the **hardware** of the original iPhone's chip
(the Samsung **S5L8900** and its ARMv6 core) precisely enough that Apple's own
**unmodified** boot ROM → iBoot → XNU kernel → `launchd` → **SpringBoard** run on
top of it, believing they're on a real 2009 iPhone.

Then it puts that whole emulated phone **inside an app on your real iPhone.**

> **The honest headline:** *no open-source emulator has ever booted iPhone OS 3.x
> to a home screen.* The closest prior art boots iPhone OS 1.1 and 2.1.1 on an
> emulated iPod touch. Booting **3.x**, targeting a real **iPhone's** SoC, is
> unclaimed ground. That's the whole point.

## Why it's different

|  | Game/app emulators | UTM (QEMU-in-app) | **iOS3-VM** |
|---|:---:|:---:|:---:|
| Runs a **real Apple OS** | ✗ | ✗ (Linux/Windows guests) | ✓ **(iPhone OS 3)** |
| **Full-system** (boots the actual kernel) | ✗ | ✓ | ✓ |
| Targets a **real iPhone's** silicon | ✗ | ✗ | ✓ (S5L8900) |
| **From scratch**, dependency-free core | — | ✗ (QEMU + glib + …) | ✓ (portable C11) |
| Runs on a **jailbroken iPhone** | some | ✓ | ✓ |

## Status

This project is built **milestone by milestone — every stage boots something you
can see.** No months in the dark.

| | Milestone | State |
|---|---|---|
| **M0** | Toolchain online: core builds + tests in CI, iOS `.ipa` builds on a macOS runner, on-device self-test runs ARM code | ✅ **done** |
| **M1** | Complete ARMv6 (ARM1176) interpreter, unit-tested | ✅ **done** — ARM + Thumb, *152 CPU tests* |
| **M2** | S5L8900 bring-up: bare-metal payload prints over emulated UART | ✅ **done** — MMU, bus, UART, VIC, timer, power, NOR/NAND · *119 SoC tests* |
| **M3** | IMG3 + NAND/NOR: Apple's real **iBoot** runs | ✅ **done** — decrypts real firmware; LLB runs, kernel extracted |
| **M4** | The real **XNU kernel** boots and logs | 🔵 **the kernel talks, and Apple's own drivers start** — reaches `bsd_init`, zero panics, zero aborts |
| **M5** | `launchd` → **SpringBoard** renders — tap it 🏆 | ⚪ next — needs a root filesystem and the display path |

### What it actually does today

Apple's real `xnu-1357.5.30`, decrypted from a stock 3.1.3 IPSW, running on
hardware that exists only as C in this repo — and introducing itself over an
emulated Samsung UART:

```
Darwin Kernel Version 10.0.0d3: Fri Dec 18 01:26:55 PST 2009;
  root:xnu-1357.5.30~6/RELEASE_ARM_S5L8900X
secure boot?: YES

Seatbelt MACF policy initialized
AppleS5L8900XIO::start: chip-revision: EVT0
AppleARMPL192VIC::start: _vicBaseAddress = 0xe397c000
AppleS5L8900XGPIOIC::start: gpioBaseAddress: 0xe3985000
AppleS5L8900XClockController: Dynamic Performance State Management Enabled
AppleS5L8900XSDIO::start(): SDIO Revision 8900X
AppleS5L8900XSPIController::start: spi0 ...
AppleS5L8900XI2CController::start: i2c0 ... i2c1 ...
```

Those are **Apple's own kernel extensions**, unmodified, matching against our
emulated device tree and programming our emulated peripherals. Nothing is
stubbed out to make that log appear — the kernel found the hardware it expected,
or it would have panicked instead.

Full detail in [`docs/ROADMAP.md`](docs/ROADMAP.md).

## How it works

```
   Windows dev box                GitHub Actions                 iPhone 6s Plus
  ┌────────────────┐   git push  ┌──────────────────┐  .ipa    ┌──────────────┐
  │ portable C core│ ──────────► │ macOS runner:    │ ───────► │ jailbroken   │
  │ tested locally │             │ xcodebuild →     │          │ iOS 15 · A9  │
  │ (MinGW, no SDK)│             │ ldid fake-sign   │          │ RWX JIT-ready│
  └────────────────┘             └──────────────────┘          └──────────────┘
```

The **emulator core is plain C11 with zero dependencies**, so it unit-tests in
under a second on any desktop — and drops unchanged into the iOS app. The only
Apple-specific code is the UIKit/Metal shell. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

**Why an old phone is the *ideal* host:** the A9 is `arm64` but predates Apple's
APRR JIT hardening (A11) and PAC/PPL (A12), so once the jailbreak relaxes
code-signing we get plain **RWX** pages from a bare `mmap` — the cleanest
possible home for a dynamic recompiler. The guests ran on a 412 MHz single core with 128 MB of RAM;
the host is a dual ~1.85 GHz `arm64` with 2 GB. Realtime is a real goal.

## Build & run

You need **nothing Apple** on your own machine — CI does the Apple build.

**Test the core locally** (any OS with a C compiler + CMake):
```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

**Get the app:** push to GitHub → the `ios-build` workflow produces an unsigned,
fake-signed `iOS3VM.ipa` artifact. Install it on your jailbroken device running
**AppSync Unified** (which lets `installd` accept the `ldid` ad-hoc-signed IPA)
via Filza or any package installer — or, on a supported device, with
**TrollStore** (which permanently re-signs it through the CoreTrust bug, no
AppSync needed). Either way, no Apple Developer account is required.

**Boot an OS** (from M3 on): drop your **own** iPhone OS 3.1.3 IPSW and its
(publicly documented) decryption keys into the app's firmware folder — see
[`docs/BOOT_CHAIN.md`](docs/BOOT_CHAIN.md).

## Requirements

- **Host:** a **jailbroken** iPhone 6s / 6s Plus (Apple A9) on iOS 15. Dopamine
  2.x (A9 support) or palera1n both work; the jailbreak's "Allow JIT in apps"
  is what unlocks the dynarec.
- **Firmware:** your own iPhone OS 3.1.3 image + keys. **None is bundled** — this
  repo contains no Apple code, exactly like a console emulator ships no ROMs.

## Legal

iOS3-VM is an original, clean-room emulator under the MIT license. It ships **no
Apple firmware, keys, or code.** You supply firmware you are entitled to use.
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

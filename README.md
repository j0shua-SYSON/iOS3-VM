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
> `mDNSResponder`. The CLI now streams the root filesystem directly into its
> checked final guest-RAM range instead of retaining a second image-sized host
> buffer. The installable iOS app does **not** run that path yet: it runs a small
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
| Apple kernel and root filesystem | Historical private-firmware run | Not integrated |
| Display | Kernel console and CLCD capture | CoreGraphics demo bridge |
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
| **M5** | `launchd` → **SpringBoard** renders — tap it 🏆 | 🔵 **in progress.** A current checkpoint chain reached the configured 2.98-billion retired-instruction cap without a guest panic or emulator undefined stop and wrote a 2.97 B checkpoint. The 2.4–2.8 B interval recorded `systemShutdown false`; the 2.85–2.98 B interval recorded two `_load_machfile` hits. No SpringBoard frame or touch path has been demonstrated; the iOS app remains a demo host. |

At `b669543`, hosted [`ios-build` run 29836528667](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/29836528667)
and [`core-tests` run 29836528681](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/29836528681)
both completed successfully. Those runs verify build, package and public tests;
they do not contain private firmware or prove a SpringBoard boot.

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

The current checkpoint chain first restored the real guest at **2.2 billion**
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
DRAM direct map. The practical route is a narrow, writable, range-checked bulk
copy bridge for md strategy I/O, with the backing identity and overlay included
in snapshots. That remains a major device-memory prerequisite.

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

**Boot the kernel** once you have supplied firmware (see below):
```sh
build/core/bootkernel firmware/kernel.macho \
    -d firmware/devicetree.bin \
    -c "debug=0x8 serial=1 nand-enable-adm=0" \
    -r firmware/rootfs.img -R 512 \
    -n 400000000
```

`-R 512` is load-bearing rather than a preference: `arm_vm_init` hardcodes
`virtual_avail = 0xe0000000`, so advertising more than 512 MiB of guest RAM makes
the kernel's zone bootstrap fault at the documented virtual base. The current
machine also rejects a larger aperture because SDRAM is exactly
`[0x08000000, 0x28000000)` and NOR starts at `0x28000000`. Historical 768 MiB
experiments are therefore not valid current recipes. `nand-enable-adm=0` keeps
`AppleS5L8900XADMFMC` from panicking on a NAND controller we do not model. Two
further workarounds (the IORTC wait, un-matching the MBX GPU driver) are applied
automatically and printed in the run header, so they are never invisible.

The large input path is host-memory bounded: `bootkernel` sizes and validates
the complete guest layout first, allocates guest DRAM once, and streams the
rootfs into its final range through the same retained source handle. It verifies
the source metadata around the read and never mirrors the roughly 445 MiB grown
RAM disk in a second host allocation.

Two more are applied to the **loaded copy** of the RAM disk — `firmware/rootfs.img`
itself is never written — and are likewise printed on every run. The guest's
`/private/etc/fstab` is retargeted at `md0`, because the stock record names a
`disk0` that only exists behind the undocumented NAND stack (`--keep-fstab` to
watch launchd halt instead). And the volume is **grown** by `--grow` MB, default
32: Apple sizes the system dmg exactly to its contents — `freeBlocks == 0` —
because on hardware everything writable lives on `disk0s2`, which this machine
does not have, so without this launchd and the daemons cannot create a single
file. It comes out of the guest's free page pool 1:1; `-Y` (RAM disk below the
kernel) recovered that space only in historical 768 MiB experiments that current
source rejects for overlapping NOR. At the valid 512 MiB physical ceiling it is
not a usable headroom recipe. `docs/BOOTLOG.md` has the numbers and the TN1150
detail.

### The tools

| | |
|---|---|
| `bootkernel` | boots the kernelcache and reports where it stopped: milestone probes, a sampled profile, every non-RAM page touched with the PC that touched it, abort sites, and the guest's console output |
| `bootkernel -L` | print the prelinked kext load map and exit without booting |
| `bootkernel --snapshot-at <insn> <file>` / `--restore <file>` | save and resume the currently modelled machine. The current chain restored at 2.2, 2.4, 2.7 and 2.85 B, wrote checkpoints at 2.4, 2.7, 2.85 and 2.97 B, and reached a clean 2.98 B cap after clearing the `UXTB16` stop; older timing numbers are host/commit-specific |
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
the published RootFS key; `bootkernel` loads that HFSX volume as a RAM disk
(`-r`). The IPA has no firmware importer or real-guest session yet, so these
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

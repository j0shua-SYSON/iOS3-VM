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
| **M1** | Complete ARMv6 (ARM1176) interpreter, unit-tested | ✅ **done** — ARM + Thumb, *229 CPU assertions* |
| **M2** | S5L8900 bring-up: bare-metal payload prints over emulated UART | ✅ **done** — MMU, bus, UART, VIC, timer, power, CLCD, NOR/NAND · *125 SoC assertions* |
| **M3** | IMG3 + NAND/NOR: Apple's real **iBoot** runs | ✅ **done** — decrypts real firmware; LLB runs, kernel extracted |
| **M4** | The real **XNU kernel** boots and logs | ✅ **done** — the whole IOKit driver tree starts, the real 413 MB root filesystem **mounts**, `_panic` is never reached |
| **M5** | `launchd` → **SpringBoard** renders — tap it 🏆 | 🔵 **in progress.** `launchd` executes user-mode code and issues its first system calls — then the boot stops on a VFP instruction our interpreter does not implement. Five syscalls is not a userland |

### What it actually does today

Apple's real `xnu-1357.5.30`, decrypted from a stock 3.1.3 IPSW, running on
hardware that exists only as C in this repo — mounting a real root filesystem
and introducing itself over an emulated Samsung UART:

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

Those are **Apple's own kernel extensions**, unmodified, matching against our
emulated device tree and programming our emulated peripherals. Nothing is
stubbed out to make that log appear — the kernel found the hardware it expected,
or it would have panicked instead.

### And here is exactly where it stops

Being precise about this matters more than the log above. `bootkernel`'s
milestone probes, from a boot with the real root filesystem:

```
_load_init_program        first @ 230,864,582
_execve                   first @ 230,895,729
_grade_binary             hits 3
_load_machfile            first @ 231,011,045
_ubc_cs_blob_add          hits 2
_cs_validate_page         hits 15
cs_validate:hashing       hits 15
cs_validate:bad_hash      NEVER REACHED
_cs_invalid_page          NEVER REACHED
_fleh_swi                 hits 24    first @ 233,031,366
_mach_msg_overwrite_trap  hits 12
_unix_syscall             hits  5    first @ 234,013,919
_panic                    NEVER REACHED

stopped after 234,731,493 instructions: UNDEFINED INSTRUCTION
  encoding at pc: 0xecb10a20 (ARM)
  lr 0xc006ae0d (_vfp_trap+0x38)
```

**pid 1 is executing user-mode code and making system calls.** That is the first
half of M5's first criterion. Then the run ends — not on a guest panic, but
because *we* stopped: the kernel took an undefined-instruction trap into
`_vfp_trap`, and the VFP encoding it went to emulate is one the interpreter does
not implement, so the machine halts at the instruction instead of guessing. That
is the M1 rule working exactly as designed, and it is the next thing to build.

Keep the scale honest: **five system calls is not a userland.** SpringBoard needs
daemons, a display controller (`AppleH1CLCD`), a panel ID, and a multitouch
device, none of which exist yet.

Getting this far needed one more emulator-shaped bug worth naming, because it
looked exactly like a corrupt disk. launchd's first text page was failing its
code-signature hash. The bytes were fine — every one of the 155 signed Mach-Os
and 6,731 code pages on the volume hashes correctly. `cs_validate_page` hashes
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
the host is a dual ~1.85 GHz `arm64` with 2 GB.

**Realtime is not promised.** The design work in
[`docs/dynarec.md`](docs/dynarec.md) §10.3 puts an honest, well-built block JIT
on this host at **0.15–0.45x** of the guest's nominal 412 MHz — "SpringBoard
responds to your finger and animates at roughly quarter speed", which is the
difference between a screenshot and a demo, and is not the same as
indistinguishable from a real iPhone.

**The dynarec's honest state:** an AArch64 emitter and an ARM block translator
exist behind `-DIOS3VM_JIT=ON`, with 297 assertions, and that is roughly **15% of
a JIT that could carry a boot**. There is no code cache, no dispatcher, no
chaining and no invalidation — so nothing calls the translator yet, and a
translated block currently costs *more* than interpreting it (mean run 8.1
instructions against a ~37-instruction prologue/epilogue). Thumb is declined
entirely, which matters because Thumb is 68.95% of retired instructions in kernel
init. It is merged so it stops rotting against a fast-moving core, not because it
is nearly done. [`docs/dynarec.md`](docs/dynarec.md) §0 keeps the score.

## Build & run

You need **nothing Apple** on your own machine — CI does the Apple build.

**Test the core locally** (any OS with a C compiler + CMake):
```sh
cmake -S . -B build && cmake --build build && ctest --test-dir build --output-on-failure
```

That is 10 suites and **974 assertions** with no firmware present; drop a real
kernelcache in `firmware/` and the symbol/kext resolver runs 258 more against it,
for 1,232. The build defaults to `Release` — the interpreter is the hot loop of
everything here, and `-O3` is a measured ~3x.

**Optionally build the dynarec** (off by default, and it must stay that way —
see the status note below):
```sh
cmake -S . -B build-jit -DIOS3VM_JIT=ON && ctest --test-dir build-jit
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
`virtual_avail = 0xe0000000`, so advertising more than 512 MB of guest RAM makes
the kernel's zone bootstrap fault. `nand-enable-adm=0` keeps
`AppleS5L8900XADMFMC` from panicking on a NAND controller we do not model. Two
further workarounds (the IORTC wait, un-matching the MBX GPU driver) are applied
automatically and printed in the run header, so they are never invisible.

### The tools

| | |
|---|---|
| `bootkernel` | boots the kernelcache and reports where it stopped: milestone probes, a sampled profile, every non-RAM page touched with the PC that touched it, abort sites, and the guest's console output |
| `bootkernel -L` | print the prelinked kext load map and exit without booting |
| `bootkernel --snapshot-at <insn> <file>` / `--restore <file>` | save and resume the whole machine. Cold boot to 900 M instructions is 140 s; restore-at-200 M and continue is 34 s |
| `snapboot` | the snapshot acceptance harness — also prints a machine-derived report, because comparing two snapshot files alone lets a field the format never stores cancel out on both sides |
| `machoinfo <kernel> -k` / `-r <addr>` | dump the kext load map, or resolve one address to a kernel symbol or `<bundle-id>+0xNNNN` |
| `img3dump`, `unlzss`, `runfw` | firmware container, compression, and bare-payload tools |

`docs/debugging.md` is the procedure these add up to.

**Get the app:** push to GitHub → the `ios-build` workflow produces an unsigned,
fake-signed `iOS3VM.ipa` artifact. Install it on your jailbroken device running
**AppSync Unified** (which lets `installd` accept the `ldid` ad-hoc-signed IPA)
via Filza or any package installer — or, on a supported device, with
**TrollStore** (which permanently re-signs it through the CoreTrust bug, no
AppSync needed). Either way, no Apple Developer account is required.

**Boot an OS** (from M3 on): drop your **own** iPhone OS 3.1.3 IPSW and its
(publicly documented) decryption keys into the app's firmware folder — see
[`docs/BOOT_CHAIN.md`](docs/BOOT_CHAIN.md). Reaching the root mount additionally
needs the IPSW's root DMG decrypted with the published RootFS key; the result is
an HFSX volume the emulator loads as a RAM disk (`-r`). Nothing in `firmware/` is
ever committed — the whole directory is git-ignored.

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

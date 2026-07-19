# Roadmap

The guiding rule: **every milestone produces something observable.** We never
build for months in the dark. Each stage boots a little more of the real OS than
the last, and each is testable.

Honest status marker: **no open-source emulator has ever booted iPhone OS 3.x**
to SpringBoard — the closest prior art (devos50/qemu-ios) reaches SpringBoard
only on 1.1 and 2.1.1, on emulated iPod touch hardware. M5 is genuinely new
ground.

---

### ✅ M0 — Pipeline online *(done)*
The whole toolchain proven end-to-end before any emulation:
- Portable C ARM core builds + unit-tests locally (MinGW) **and** in CI.
- iOS app builds on a macOS runner, fake-signs, produces an installable `.ipa`.
- On-device self-test runs real ARM code through the core and reports JIT/RWX
  readiness.

**Observable:** green CI badges; the app prints `r0 = 42` computed by our CPU
core on the phone.

### 🔵 M1 — ARMv6 interpreter *(in progress)*
A complete, correct ARM1176 instruction interpreter validated against a growing
unit-test suite.
- Data-processing, branch, load/store, multiply — **done** (24 tests passing).
- Remaining: block data transfer (LDM/STM), Thumb, PSR transfer (MRS/MSR),
  coprocessor (CP15) for MMU control, SWI/exceptions, banked registers per mode.

**Observable:** the interpreter runs a known ARM test binary with bit-exact
register/memory results.

### ⚪ M2 — Bare-metal boot + UART
MMU/TLB, the system bus, and the minimum S5L8900 devices (VIC, timers, UART,
GPIO, clock) to run *our own* small bare-metal ARM payload that prints over the
emulated UART, surfaced in the app.

**Observable:** text from guest code appears on the iPhone screen. First "it runs
on the phone."

### ⚪ M3 — iBoot / SecureROM chain
IMG3 firmware parser (AES + RSA via a bundled crypto), NOR + NAND with the
FTL/VFL layers, device tree. Execute Apple's real low-level boot chain far enough
to see **iBoot** serial output.
*Requires the user to supply their own iPhone OS 3.1.3 IPSW + the public
decryption keys — see [BOOT_CHAIN.md](BOOT_CHAIN.md).*

**Observable:** genuine iBoot banner over emulated serial.

### ⚪ M4 — XNU kernel boots
Enough SoC fidelity (interrupt semantics, timers, cache/MMU maintenance ops,
ARMv6 unaligned-access quirks) for the **real iPhone OS 3 XNU kernel** to
initialize and emit console logs.

**Observable:** XNU boot log streaming from the emulated kernel.

### ⚪ M5 — launchd → SpringBoard 🏆
LCD framebuffer wired to a Metal view, multitouch mapped from the host
touchscreen, remaining devices filled in. Boot through `launchd` to a rendered
**SpringBoard** home screen you can tap.

**Observable:** iPhone OS 3's home screen, live, on a 2015 iPhone. The grail.

### ⚪ Dynarec (parallel track)
Once the interpreter is correct, add an ARMv6→ARM64 JIT emitting into RWX pages
for near-realtime speed, with the interpreter as its differential oracle.

**Observable:** SpringBoard at interactive frame rates.

---

## Deliberately out of scope (for now)
- **Telephony / baseband / SIM** — the S5L8900 emulation models the iPhone 2G/3G
  *application processor*; the modem is stubbed. No cellular, and that's fine.
- **Wi-Fi data path, audio, GPU acceleration** — later, if ever.
- **App Store distribution** — impossible by design (JIT + full-system emulation
  violate App Store policy). This is a jailbreak-only project.

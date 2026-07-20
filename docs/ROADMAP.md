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
- **Done** (83 CPU tests passing): data-processing with the full barrel shifter,
  branch/BL, BX/BLX, single load/store, multiply, **LDM/STM in all four
  addressing modes** (incl. push/pop and LDM-into-PC branching), the
  halfword/sign-extending loads (LDRH/STRH/LDRSB/LDRSH), **banked registers and
  mode switching** (per-mode r13/r14 + SPSR, FIQ's private r8–r12), **MRS/MSR**,
  and **SWI with a full exception round trip** (vector → SVC entry with SPSR/LR
  saved → `LDM ^` exception return restoring CPSR), and the **CP15 system
  control coprocessor** via MCR/MRC (MIDR, SCTLR, TTBR0/1, DACR, fault status,
  context ID; cache/TLB maintenance accepted as no-ops) — including SCTLR.V
  relocating the vector table to `0xFFFF0000`.
- Unimplemented encodings deliberately return `ARM_UNDEFINED` rather than
  silently corrupting state, so the harness tells us exactly what to add next.
  (CP15 is the documented exception: unmodelled config registers read as zero
  instead of trapping, since kernels probe that space widely.)
- Remaining: Thumb, SWP, LDRD/STRD, user-bank `LDM ^`. (IRQ/FIQ delivery and
  abort exceptions are done.) **MMU page-table walking** uses the CP15 state landed here
  but belongs to M2.

**Observable:** the interpreter runs a known ARM test binary with bit-exact
register/memory results.

### 🔵 M2 — Bare-metal boot + UART *(started)*
- **Done: the MMU.** Full ARMv6 short-descriptor translation — 1 MB sections,
  two-level coarse tables with 64 KB large and 4 KB small pages, domain checks
  via DACR, AP permission checks, and data/prefetch aborts wired into execution
  (DFSR/DFAR, IFSR/IFAR). Page tables are walked out of guest RAM through the
  normal bus, exactly as hardware does.
- **Done: the system bus and UART.** The S5L8900 memory map routes physical
  accesses to RAM or a peripheral window; unmapped accesses are *counted*, not
  silently swallowed, so gaps are visible. The Samsung-style UART captures
  everything the guest transmits — the same channel iBoot and XNU will use.
- **Done: guest code produces output.** A hand-assembled bare-metal ARM payload
  runs on the emulated SoC and prints over the UART:

  ```
  iOS3-VM S5L8900 machine tests
    [guest said] HI
  ```

- **Done: the interrupt path.** A PL190-style VIC masks and routes numbered
  lines to IRQ or FIQ; a periodic timer drives one. The CPU samples both lines
  before each fetch and takes a real exception, and handlers return with
  `SUBS pc, lr, #4` (which restores CPSR from SPSR):

  ```
  [timer IRQ -> handler -> return] uart="T", resumed at pc=00000100
  ```

- Remaining: GPIO and clock; then surface the UART stream in the iOS app.

**Observable:** text from guest code appears on the iPhone screen. First "it runs
on the phone."

### 🔵 M3 — iBoot / SecureROM chain *(started)*
- **Done: the IMG3 container parser.** Reads the header and tag stream, exposing
  the DATA payload, the KBAG (crypt state, key bits, IV and key), SHSH presence
  and the VERS string. Strictly bounds-checked with 64-bit arithmetic: this is
  the first component to touch untrusted user-supplied files, so malformed
  containers are rejected rather than read out of bounds (tests cover oversized
  fullSize, tags past the end, dataLen exceeding its tag, and the zero-length
  tag that would otherwise loop forever).
- **Done: AES-CBC decryption.** A self-contained AES (128/192/256)
  implementation, validated against the FIPS-197 known-answer vectors, wired
  into the IMG3 parser: given a user-supplied key it decrypts the DATA payload
  using the KBAG's IV, passing any unaligned tail through unchanged. No
  OpenSSL, so the core keeps its zero-dependency property.
- Remaining: NOR + NAND with the FTL/VFL layers,
  device tree. Execute Apple's real low-level boot chain far enough to see
  **iBoot** serial output.
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

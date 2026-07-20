# Roadmap

The guiding rule: **every milestone ends in something observable.** Not "the MMU
is implemented" but "the kernel builds its own page tables, enables the MMU, and
keeps running". Not "drivers work" but "here are the bytes Apple's driver
printed over our UART". If a milestone cannot be phrased as a thing you can
watch happen, it is not a milestone, it is a hope.

Everything below is measured. The numbers come from running the emulator at the
current commit against a real `iPhone1,2_3.1.3_7E18` IPSW; where a number is
historical — from the run *before* a fix — it says so.

Honest positioning: **no open-source emulator has booted iPhone OS 3.x to
SpringBoard.** The closest prior art (devos50/qemu-ios) reaches SpringBoard on
1.1 and 2.1.1, on emulated iPod touch hardware. M4 is at the edge of what has
been done publicly; M5 is new ground.

---

## Status at a glance

| | Milestone | Observable completion criterion | State |
|---|---|---|---|
| **M0** | Pipeline online | CI green on both jobs; the app prints a value computed by our CPU core on the phone | ✅ done |
| **M1** | ARMv6 interpreter | ARM + Thumb; unimplemented encodings trap instead of guessing | ✅ done — *144 CPU tests* |
| **M2** | SoC bring-up | A bare-metal payload prints over the emulated UART; a timer IRQ is taken and returned from | ✅ done — *47 machine tests* |
| **M3** | Apple's firmware chain | Real IMG3s parse and decrypt; real Apple LLB executes; the kernelcache is extracted | ✅ done |
| **M4** | XNU boots and logs | The kernel reaches `bsd_init`, prints, and Apple's own kexts match and start | 🔵 **all but the root device** |
| **M5** | Userspace → SpringBoard | `launchd` runs; the home screen renders and takes a tap | ⚪ next |
| **D** | Dynarec (parallel) | SpringBoard at interactive frame rates on the phone | ⚪ designed, not built |
| **N** | Guest networking (parallel) | The guest resolves a name and fetches a URL | ⚪ designed, not built |

---

## ✅ M0 — Pipeline online

**Criterion:** both CI jobs green, and an `.ipa` that runs real ARM code through
our core on the device.

- Portable C11 core builds and unit-tests locally (MinGW/GCC) and in CI.
- The iOS app builds on a macOS runner, fake-signs with `ldid`, produces an
  installable `.ipa` — no Apple Developer account anywhere in the loop.
- An on-device self-test runs ARM instructions through the interpreter and
  drives the emulated timer → VIC → CPU interrupt path, then reports JIT/RWX
  readiness.

**Observed:** `r0 = 42`, computed on the phone by our interpreter.

---

## ✅ M1 — ARMv6 interpreter

**Criterion:** a complete ARM1176 interpreter in which every unimplemented
encoding *traps* rather than executing something plausible.

Done, and since validated by ~200 million instructions of Apple's own code per
boot: the full data-processing set with the barrel shifter, branches and
interworking, all load/store forms including LDM/STM in four addressing modes,
the halfword and sign-extending loads, long multiplies
(`UMULL`/`UMLAL`/`SMULL`/`SMLAL`), banked registers and mode switching,
`MRS`/`MSR`, the whole exception-entry and exception-return family (`SWI`,
`SUBS pc,lr`, `MOVS pc,lr`, `LDM {..,pc}^`, `RFE`, `SRS`), CP15 and CP14, the
ARMv6 media and extend instructions, the complete ARMv6K exclusive family
(`LDREX`/`STREX` plus the B/H/D variants, `CLREX`, `SWP`/`SWPB`), and the Thumb
instruction set with ARM↔Thumb interworking.

**The rule that makes this milestone worth anything:** an encoding we have not
implemented returns `ARM_UNDEFINED` and stops the machine. It never falls
through to "close enough". CP15 is the one documented exception — unmodelled
config registers read as zero, because kernels probe that space far too widely
to trap on it.

**Observed:** 144 CPU unit tests, green, in under a tenth of a second.

---

## ✅ M2 — SoC bring-up and UART

**Criterion:** guest code running on the emulated S5L8900 prints text, and an
interrupt is delivered and returned from.

- Full ARMv6 short-descriptor MMU: 1 MB sections, supersections, coarse tables
  with 64 KB large and 4 KB small pages, domain checks via DACR, AP permission
  checks, and data/prefetch aborts wired into execution (DFSR/DFAR, IFSR/IFAR).
  Page tables are walked out of guest RAM through the normal bus, exactly as
  hardware does.
- A system bus with the S5L8900 memory map. Accesses that hit nothing are
  **counted, attributed and reported**, never silently swallowed.
- A Samsung-style UART, a PL190/PL192-style VIC with per-line IRQ/FIQ routing,
  and the real S5L8900 timer block.

**Observed:**

```
iOS3-VM S5L8900 machine tests
  [guest said] HI
  [timer IRQ -> handler -> return] uart="T", resumed at pc=00000100
```

47 machine tests, green.

---

## ✅ M3 — Apple's firmware chain

**Criterion:** real, unmodified Apple firmware out of a real IPSW parses,
decrypts, and *executes*.

- IMG3 container parser — every tag present in genuine 7E18 firmware
  (`TYPE`/`DATA`/`VERS`/`SEPO`/`BORD`/`KBAG`/`SHSH`/`CERT`) is one we handle.
  Bounds-checked in 64-bit arithmetic, because this is the first component to
  touch a user-supplied file.
- A self-contained AES-128/192/256 validated against the FIPS-197 known-answer
  vectors, plus LZSS for the kernelcache. No OpenSSL; the core keeps its
  zero-dependency property.
- Apple's own device-tree format (not FDT): node and property traversal, path
  lookup, depth-limited and bounds-checked against malicious input.
- NOR flash with IMG3 scanning, writable and persistent (the shape an untethered
  jailbreak needs); NAND with real flash semantics — erased pages read as ones,
  programming can only clear bits.

**Observed:** the real version strings come out of the user's own IPSW
(`iBoot-636.66.33`, `EmbeddedDeviceTrees-390.16`), and Apple's **LLB executes
6,668 instructions** of genuine code — switching to Thumb, making real function
calls, touching zero unmapped addresses — before failing a header check on flash
we had not yet populated.

**Deliberately not done: Apple's VFL/FTL.** Mapping logical to physical NAND
pages faithfully requires validating against real firmware behaviour; a
plausible-looking guess would pass our tests and fail silently on a genuine NAND
dump, which is worse than having none. The raw device is provided for an FTL to
sit on later, and the gap is stated in `nand.h` rather than papered over.

---

## 🔵 M4 — XNU boots and logs

**Criterion:** the real iPhone OS 3.1.3 kernel initialises, prints to the
console, and Apple's own kernel extensions match against our device tree and
program our emulated peripherals — with no panic.

**That criterion is met.** A 200,000,000-instruction boot of
`xnu-1357.5.30~6/RELEASE_ARM_S5L8900X`, decrypted from a stock IPSW:

| Measurement | Value |
|---|---|
| `_panic` / `_Debugger` reached | **never** |
| `_bsd_init` reached | instruction 66,757,032 |
| Console output | **2,177 bytes** (2,177 `_uart_putc` calls) |
| Distinct functions executed (sampled) | 744 |
| `_DTGetProperty` calls | 858 — IOKit walking our device tree |
| FIQ entries / cost | 91 / 2,997 instructions (0.0% of the run) |
| `STREX` executed / failed | 987,862 / 0 |
| Exception returns into Thumb | 108, of which 64 land 4-byte aligned |
| Non-RAM physical pages touched | 13 |
| Accesses to pages we do not model | 19 reads, 772 writes |

Apple's kexts, unmodified, announcing themselves over a UART that exists only as
C in this repository:

```
Darwin Kernel Version 10.0.0d3: Fri Dec 18 01:26:55 PST 2009;
  root:xnu-1357.5.30~6/RELEASE_ARM_S5L8900X

Seatbelt MACF policy initialized
AppleS5L8900XIO::start: chip-revision: EVT0
AppleARMPL192VIC::start: _vicBaseAddress = 0xe38ed000
AppleS5L8900XEdgeIC::start: _edgeicBaseAddress: 0xe38e6000
AppleS5L8900XGPIOIC::start: gpioBaseAddress: 0xe38f5000
AppleS5L8900XPowerController::start: _pcBaseAddress: 0xe38fd000
AppleS5L8900XClockController: Dynamic Performance State Management Enabled
AppleARMPL080DMAC::start: dmac0 / dmac1
AppleS5L8900XADM::start: mapped I/O registers at 0xe9915000/0x38800000
AppleS5L8900XSDIO::start(): SDIO Revision 8900X
AppleS5L8900XSPIController::start: spi0 / spi1
AppleS5L8900XUSBPhy::start registers at 0xea942000
AppleS5L8900XI2CController::start: i2c0 / i2c1
AppleS5L8900XTimer / AppleS5L8900XWatchDogTimer / AppleS5L8900XI2SController
AppleMBXDevice(0xc0bf4800): Init
ApplePCF50635PMU::start / AppleMicron2020::start()
Registering IOCameraSensor service.
```

An annotated walk through this exact boot, stage by stage, with the emulated
device each driver is talking to, is in [BOOTLOG.md](BOOTLOG.md).

### And the first pixels

The kernel now paints its own boot log into the framebuffer. `initialize_screen`
was being reached all along; the blocker was that `boot_args.v_display` was set
non-zero, which makes `vcattach()` return early, so the graphics console was
never acquired. With `v_display = 0` — and `serial=1` dropped from the command
line — the kernel takes the framebuffer for itself: **61,659 of 614,400 bytes
non-zero, 20,553 lit pixels across 313 rows**, verified by rendering the buffer
to an image and reading the text back off it:

```
iBoot version:
Seatbelt MACF policy initialized
AppleS5L8900XClockController: Dynamic Performance State Management Enabled…
AppleS5L8900XSDIO::start(): SDIO Revision 8900X
AppleMBXDevice(0xc0bcf800): Init
AppleMicron2020::start()
Registering IOCameraSensor service.
```

That is XNU's own graphics console, drawn glyph by glyph by the kernel into
memory we handed it — on a display controller that does not exist yet.

### The five bugs that got us here

Each was found in the last stretch of work, and each is worth naming for *how*
it hid.

1. **The timer block was invented.** Ours was a plausible four-register device
   at 0x00/04/08/0c. The kernel's own symbols said otherwise: a free-running
   64-bit counter at 0x80/0x84 (this is `mach_absolute_time`), timer 4 at
   0xA0–0xAF, and VIC line 7 routed to **FIQ**, not IRQ. Nothing the kernel
   touched existed, so `mach_absolute_time()` read zero forever, every delay
   loop waited on a dead clock, and the boot died silently in a spin rather than
   panicking. Fixing it produced the first console output this project ever
   made: `iBoot version: `.

2. **The framebuffer was in the wrong place.** It sat immediately after
   `boot_args`, and `topOfKernelData` was then advanced past it — which moved
   where the kernel places its own page tables, and turned into a prefetch abort
   39,767 instructions in. Real iBoot puts the framebuffer near the top of DRAM.
   So do we now, and `topOfKernelData` describes only the kernel's data again.

3. **Exception returns word-aligned Thumb resume addresses.** Writing PC with S
   set copies SPSR into CPSR — restoring the interrupted mode *and* its T bit —
   and the next line then did `*next = res & ~3u`, unconditionally. Returning
   into Thumb code at an address 2 mod 4 rewound two bytes and re-executed the
   preceding halfword. It presented nowhere near its cause: a decrementer FIQ
   landed inside `_zfree`, the return rewound into the tail of the *locked*
   path, and the kernel unlocked a mutex at address 1 — a data abort at
   `_lck_mtx_unlock+0x8` with DFAR 0x1, deep inside IOKit. The tell was
   statistical rather than local: **761 of 761** exception returns into Thumb
   resumed at a 4-byte-aligned address, where hardware would be roughly half and
   half, and 372 of them (48.9%) needed a +2 correction. Two competing
   hypotheses — a failing exclusive monitor, an uninitialised IOKit structure —
   were instrumented and *refuted* rather than assumed: 70,008 exclusive stores
   executed in that boot, zero failed.

4. **Guest time ran 68× fast.** The timebase advanced once per retired
   instruction. On the hardware the CPU runs at 412 MHz and the timebase counts
   at 6 MHz — one tick per ~68 cycles — so the kernel could never finish
   servicing a decrementer deadline before the next was already in the past. It
   clamped the decrementer to its minimum and re-entered, forever. Measured
   before: **1,939,179 FIQ entries, 131,864,057 instructions inside FIQ, 65.9%
   of the run**. After converting instructions to ticks at the real cpu:tb ratio
   (carrying the remainder so it stays exact instead of drifting): **86**. Both
   rates are fields rather than constants, because a ratio that decides whether
   emulated hardware looks implausibly fast to the guest deserves to be a knob,
   not a buried assumption.

5. **The power-gate controller.** `AppleS5L8900XPowerController::start` writes
   the domains it wants gated and then spins until `STATE` agrees:
   `write(OFFCTRL, 0x12fc); do { s = read(STATE); } while ((s & 0x12fc) != 0x12fc);`
   Unmodelled, `STATE` read 0 forever — **3,887,707 reads** — and `start()`
   never returned, so the controller never registered and nothing downstream
   could power-gate anything. It is a real device model rather than a stub for a
   concrete reason: the guest never *writes* `STATE`, so read-back storage would
   return zero just as forever. Polarity came from the driver's own generic gate
   routine — power-up writes `ONCTRL` and waits for the bit to **clear**,
   power-down writes `OFFCTRL` and waits for it to **set** — not from a guess.
   With the model in place the whole page takes 7 reads in a 200M-instruction
   boot, and a wave of drivers came up behind it: I2S, SPI1, MBX, SDIO, the PMU,
   the camera sensor. Console output went 1,673 → 2,177 bytes.

Two smaller ones, just as instructive. `CLREX` was being silently swallowed by
the `PLD` decode — `0xF57FF01F` satisfies the PLD pattern, so with PLD tested
first `CLREX` became a hint that did nothing, which would let a `STREX` the
architecture requires to fail succeed instead: two threads holding one lock. And
in the long multiplies, bit 22 selects **signed**, and it was inverted — caught
only by a test that multiplies `0xFFFFFFFF` by itself both ways, because for
small operands the signed and unsigned answers agree.

### What is actually left in M4

- **No root filesystem, so no userspace.** The kernel reaches `bsd_init` and
  then has nothing to mount. `bootkernel` can already publish a RAM disk through
  `/chosen/memory-map` and append `rd=md0`; what is missing is a root image the
  3.1.3 kernel will accept.
  **Next observable:** the kernel printing `Still waiting for root device` —
  which reads like a failure and is the opposite. It is what XNU prints when
  everything above the filesystem is up and healthy. (The string is present in
  the kernelcache; we have not yet made it appear.)
- **48 distinct abort sites remain.** All are data aborts with FSR 0x07 (page
  translation fault), in `IOBufferMemoryDescriptor::initWithPhysicalMask` and
  the kernel's own `_fleh_dataabt`, the first at instruction 132,883,590 with
  DFAR 0xea93a000. The kernel takes them and carries on — there is no panic —
  but they are unexplained, and "unexplained but survivable" is precisely the
  category that becomes a mysterious failure three milestones later.
- **19 reads and 772 writes go to pages we do not model** — 10 distinct pages:
  the edge interrupt controller, GPIO, the second VIC, the clock/reset
  generator, SDIO, i2c0, and three we have not yet identified. They are counted
  and attributed to a PC, which is the point; but each one is a driver talking
  to a device that is not listening.
- **`AppleH1CLCD` does not start.** The display controller is
  `/device-tree/arm-io/clcd`, `compatible = "clcd,s5l8900x"`, physical
  0x38900000, interrupt 13. Nothing models it, so the kext never comes up — and
  it is the piece that turns a framebuffer into a display.
- **`AppleMerlotLCD` needs a panel ID.** `/device-tree/arm-io/spi0/lcd0` is
  `compatible = "lcd,merlot"` with `lcd-panel-id = 0x00000000`. Real iBoot reads
  the panel's ID over SPI and patches it in; we stand in for iBoot, so we have
  to write a non-zero one — the same in-place, same-length device-tree patch we
  already perform for the clock frequencies.

---

## ⚪ M5 — userspace → SpringBoard 🏆

**Criterion, in order, each independently observable:**

1. The kernel mounts a root filesystem and `launchd` executes its first
   instruction in user mode.
2. Daemons start, and the system log shows them doing it.
3. SpringBoard renders the home screen into the framebuffer.
4. A touch delivered from the host's screen moves something on the guest's.

What M5 needs that does not exist yet:

- **A root filesystem** — Apple's ramdisk or root DMG from the IPSW, or a
  synthesised minimal root. This is the gating item.
- **The display path** — `AppleH1CLCD` plus the Merlot panel, then the guest
  framebuffer surfaced through the app's Metal view.
- **Multitouch**, mapped from the host touchscreen to the guest's controller.
- **NAND VFL/FTL**, if we mount a real NAND image rather than a RAM disk.

### Two things that could kill M5 outright, neither investigated yet

Stated up front because they are the honest risks, not because we have answers.

- **Activation.** A stock iPhone OS 3 sits at "connect to iTunes" until
  `lockdownd` sees an activation record. Whether SpringBoard is reachable
  without one — by supplying a record, by patching, or at all — has not been
  looked at.
- **Guest code signing.** 3.1.3 enforces signing on userspace binaries. The
  kernel does contain `cs_enforcement_disable`, `amfi_get_out_of_my_way` and
  `PE_i_can_has_debugger`, and `/chosen/debug-enabled` is a 4-byte property
  currently zero — exactly the shape our device-tree patcher already writes.
  That is encouraging. It is not the same thing as having tried it.

**Performance is a quantified limitation rather than a risk.** The interpreter
runs a tight synthetic loop at roughly 14 million instructions per second on the
development host, against a guest CPU that ran at 412 MHz; real code with MMU
translation is slower still, and the phone is not faster than the desktop. That
is one to two orders of magnitude short of realtime — enough to *render* a
SpringBoard, not to *use* one. Which is what the dynarec is for.

---

## ⚪ Parallel tracks

### Dynarec

An ARMv6→ARM64 JIT emitting into RWX pages, with the interpreter as its
differential oracle: both share the same semantics module, so the JIT can be
checked instruction by instruction against it. The A9 host is chosen partly for
this — it predates APRR (A11) and PAC/PPL (A12), so on a jailbroken device we
get plain RWX from a bare `mmap`.

**Observable:** SpringBoard at interactive frame rates.

### Guest networking

Designed in [networking.md](networking.md), not built. A paravirtual Ethernet
NIC with our own guest kext delivered as a boot-time mkext, and host egress
through a userspace NAT onto ordinary BSD sockets — no root, no `utun`, no
reconfiguring the phone. Three expected blockers were each settled against this
project's own firmware rather than by argument: the kext-loading path exists in
the kernel, the device tree has spare `MemoryMapReserved-*` slots to deliver it
through, and the host needs no privilege. The biggest risk is stated there
rather than buried: Apple never shipped a Kernel.framework for iPhoneOS, so we
need an `MH_KEXT_BUNDLE` whose C++ vtable layout matches a GCC-4.2-built 2009
kernel.

**Observable:** the guest resolves a hostname and fetches a URL over plain HTTP.

---

## What we learned

There is one lesson under nearly every bug in this project, and it is not
"ARMv6 is fiddly".

**Every bug found here was invisible until an unrelated fix unlocked the code
path that exposed it.** Not merely hard to find — *invisible*, because the code
containing it had never executed.

- Clearing the exclusive monitor on exception entry was wrong for a long time
  and could not matter, because no interrupt had ever fired. It became a real
  bug the instant the timer started working.
- The `MOVS pc,lr` alignment bug is dormant without interrupts, for the same
  reason. It needed the timer fix to become reachable — and then it did not
  announce itself either, presenting as a data abort on address 1 in a different
  subsystem, millions of instructions later.
- The timebase ratio was harmless while nothing armed a decrementer.
- The power controller could not be discovered until the drivers ran, and the
  drivers could not run until the kernel stopped panicking.
- Even the diagnostics had this shape. The FIQ logger sampled the first twelve
  interrupts, all healthy at 55,245-instruction spacing — and the storm began at
  instruction 66 million. The profiler silently dropped every function past its
  64-entry table, so it printed identical output at 200M and 400M instructions
  and looked exactly like coverage.

The practical consequence is that **progress here is not linear and cannot be
planned as though it were.** Every fix is also an excavation: it exposes code
that has never run on this emulator, and that code contains bugs which have
never had the opportunity to be wrong. The useful question is not "how many bugs
are left" but "what does the next unlocked path let us see".

What makes that survivable is the rule from M1: **trap what you do not
implement, never guess.** It is the difference between a bug and a mystery.

- An unimplemented instruction stops the machine *at* the instruction instead of
  computing something plausible and corrupting state that fails somewhere else.
  That is how `UMULL`, `LDREXD` and the Thumb `BLX` suffix were found — the
  emulator named them.
- Unmapped bus accesses are counted and attributed to a PC and a kernel symbol,
  so "which device does the kernel want next" is a report rather than a guess.
  That is how the real timer register map was recovered — not from a datasheet,
  but by logging what the kernel touched and correlating it against the kernel's
  own symbol table. `_s5l8900x_get_timebase` reading 0x080/0x084 told us more
  than any documentation could have.
- The one place the rule is deliberately relaxed — named MMIO stub windows — is
  bounded and argued, because for an MMIO read there *is* no neutral option:
  returning 0 is already a guess, and a demonstrably dangerous one. So a stub is
  honest storage (reads return what was written), and it is named and counted so
  the gap appears in the report instead of hiding among real faults. What a stub
  must never do is fabricate a value the guest is waiting for; when a driver
  needs a bit to change on its own, that is a device model, not a stub.

The corollary is uncomfortable and worth stating plainly: **a green test suite
proves nothing about the paths you have not reached.** All 8 suites were green
throughout every bug above. What found them was running real firmware and
insisting the emulator complain loudly the moment it was asked for something it
did not have.

---

## Deliberately out of scope

- **Telephony, baseband, SIM.** We emulate the application processor; the modem
  is stubbed. `AppleBaseband: Could not find mux function` in the boot log is
  us, on purpose.
- **Wi-Fi through the real Marvell 88W8686**, audio output, GPU acceleration.
  "Route A" for networking — emulating the real NIC so Apple's driver binds
  unmodified — is fully specified in `networking.md` and deliberately deferred.
- **App Store distribution.** Impossible by design: a JIT and full-system
  emulation each violate App Store policy on their own. This is a jailbreak-only
  project, and that is the point rather than a limitation.

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
| **M0** | Pipeline online | CI green (core tests on ubuntu + macOS, an `IOS3VM_JIT=ON` job across ubuntu/macos-14/macos-15, an asan+ubsan job, and the iOS `.ipa` build); the app prints a value computed by our CPU core on the phone | ✅ done |
| **M1** | ARMv6 interpreter | ARM + Thumb; unimplemented encodings trap instead of guessing | ✅ done — *229 CPU assertions* |
| **M2** | SoC bring-up | A bare-metal payload prints over the emulated UART; a timer IRQ is taken and returned from | ✅ done — *125 machine assertions* |
| **M3** | Apple's firmware chain | Real IMG3s parse and decrypt; real Apple LLB executes; the kernelcache is extracted | ✅ done |
| **M4** | XNU boots and logs | The kernel reaches `bsd_init`, prints, and Apple's own kexts match and start | ✅ **done** — plus the real root filesystem mounts |
| **M5** | Userspace → SpringBoard | `launchd` runs; the home screen renders and takes a tap | 🔵 **in progress — `launchd` executes user-mode code and issues 5 system calls**, then the boot stops on a VFP encoding the interpreter does not implement |
| **D** | Dynarec (parallel) | SpringBoard at interactive frame rates on the phone | 🔵 emitter + block translator exist (J2, off by default); no code cache, no dispatcher — nothing calls it yet |
| **N** | Guest networking (parallel) | The guest resolves a name and fetches a URL | ⚪ designed, not built |

Test totals, measured at this commit: **974 assertions across 10 suites** with no
firmware present, **1,232** when a real kernelcache is in `firmware/` (258 of the
`ksyms` assertions run only against real bytes), plus **297** in the two dynarec
suites that the `IOS3VM_JIT=ON` CI job builds.

---

## ✅ M0 — Pipeline online

**Criterion:** CI green, and an `.ipa` that runs real ARM code through our core on
the device.

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

**Observed:** 229 CPU assertions, green, in under a tenth of a second.

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

125 machine assertions, green.

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

## ✅ M4 — XNU boots and logs

**Criterion:** the real iPhone OS 3.1.3 kernel initialises, prints to the
console, and Apple's own kernel extensions match against our device tree and
program our emulated peripherals — with no panic.

**That criterion is met, and the milestone is complete.** A
400,000,000-instruction boot of `xnu-1357.5.30~6/RELEASE_ARM_S5L8900X`,
decrypted from a stock IPSW, with the real 413 MB root filesystem attached as a
RAM disk. (Measured at `9363283`. At HEAD the same command no longer *reaches*
400 M: pid 1 now makes progress instead of spinning, and the run halts at
234,731,493 on an unimplemented VFP encoding — see M5.)

| Measurement | Value |
|---|---|
| `_panic` / `_Debugger` reached | **never** |
| `_bsd_init` reached | instruction 64,567,734 |
| Root filesystem | **`BSD root: md0, major 2, minor 0`** |
| Console output | **4,595 bytes** |
| Distinct functions executed (sampled) | 1,024 (the profiler's table is now the limit, and says so) |
| `_DTGetProperty` calls | 858 — IOKit walking our device tree |
| FIQ entries / cost | 385 / 38,235 instructions (0.0% of the run) |
| `STREX` executed / failed | 2,715,561 / 13 — all in `lck_mtx_*`, i.e. real contention |
| Exception returns into Thumb | 351, of which 204 land 4-byte aligned (~58%; hardware would be ~50%) |
| Non-RAM physical pages touched | 22 |

This table replaces the earlier 200 M-instruction figures (2,177 bytes of
console, 13 device pages, 91 FIQs), which were measured before the root mount and
before the TTBR1 fix. Where the older numbers still appear below, they are
labelled as historical.

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

### The walls cleared after `bsd_init`, in order

This chain is the most useful record in the repository, so it keeps going. Each
entry is a wall that stopped the boot dead, and how it was found.

1. **The interrupt storm.** The kernel raised a self-IPI on VIC software-interrupt
   line 4 and read `VICADDRESS` (0xF00) to find the source; our stub returned 0,
   which the PL192 driver decodes as spurious source 0, so the IPI was
   acknowledged but never cleared and `_fleh_irq` re-fired forever. Fixed by
   modelling `VICADDRESS` to return `source | 0x80000000`. Console output jumped
   2,177 → 8,191 bytes. (`8ebeb2a`)
2. **`AppleS5L8900XADMFMC::start` panic** ("ADM startup failed"). The NAND/DMA
   driver's `admStart` polls the ADM status register for a ready bit that never
   sets in emulation. Since we boot from a RAM disk we do not need NAND, and the
   driver's own `probe()` honours the boot-arg **`nand-enable-adm=0`** — with it,
   the driver never matches and never panics. That boot-arg is now part of the
   standard recipe.
3. **The MBX GPU driver.** `AppleMBX` matched `/arm-io/mbx` and wedged on an
   unmodelled 2D/3D block. Breaking that node's `compatible` string so nothing
   matches clears the graphics wedge and the boot goes idle instead; iPhone OS 3
   has a software-blit path, so the GPU is not required. (`559b633`)
4. **The IORTC wait.** `bsd_init` → `IOKitInitializeTime` waits 30 seconds for a
   service named `IORTC`, which is never published because the PMU's RTC is not
   modelled. Patching that timeout to zero reaches `IOFindBSDRoot`, and the
   kernel **mounts the RAM disk**: `BSD root: md0, major 2, minor 0`. (`9e29149`)
5. **512 MB is a hard ceiling, and it is the kernel's, not ours.** Booting the
   real 413 MB rootfs with `-R 768` panicked at ~34 M in early VM init with a
   null-zone dereference. `arm_vm_init` hardcodes `virtual_avail = 0xe0000000`,
   so the kernel's physical-linear window is exactly 512 MB; advertising more
   makes `zone_virtual_addr` stop early-outing and index a `pv_head_table` that
   is still entirely zero during `zone_bootstrap`. Capping the advertised
   `memSize` at 512 MB is what makes the real root filesystem mount. Real
   S5L8900 devices shipped ≤256 MB, so hardware never reaches this path.
   (`5625f5c`)
6. **ARMv6 `TTBCR.N` / `TTBR1` — the first genuinely systemic bug.**
   `arm_mmu_translate` walked TTBR0 unconditionally. ARMv6 splits translation
   between TTBR0 and TTBR1 at a boundary set by `TTBCR.N`, and this kernel runs
   with **N=2**: the only two `MCR p15,0,Rd,c2,c0,2` sites in the entire binary
   both write the literal 2, and `set_mmu_ttb` writes TTBR0 *alone*. So kernel
   text at 0xc0008000–0xc020d000 and the 0xffff0000 vector page live in TTBR1,
   while TTBR0 holds the current user pmap. Walking TTBR0 always was survivable
   only while both TTBRs happened to hold the same base — the first `pmap_switch`
   to a user pmap deleted kernel text and the vector page from the walk, and the
   CPU stormed on prefetch aborts at 0xffff000c forever. **That was the
   long-standing "unsymbolized kext spin".** With the split honoured the boot
   runs the entire driver tree: timers, I²C, I²S, SPI, USB PHY, twelve DMA
   channels, uart0/1/3/4, the spi-baseband mux, `AppleMultitouchZ2SPI`,
   `AppleMobileFileIntegrity`, `ApplePCF50635PMU`. Seven tests cover the N=0
   regression guard, the N=2 geometry, N=1/N=3 to prove the formulas scale, that
   a TTBR1 miss does not fall back to TTBR0, and the actual bug; they fail 17
   checks against the pre-fix walker. (`e97934d`, hardened in `aa4f0c5`)
7. **`DFSR.WnR` was never set — the second systemic bug.** Bit 11 says "the abort
   was caused by a write access", and XNU's `sleh_abort` derives `fault_type`
   from `tst r2,#0x800`. With the bit clear it always took the read path,
   rewrote the PTE with `AP=0b10` (privileged RW, user read-only), and returned
   `KERN_SUCCESS`. The faulting unprivileged store re-ran, hit the same
   permission fault, and the kernel repaired it the same wrong way — **~2.8
   million identical aborts** at `_copyout+0x40`, one every ~395 instructions,
   zero user-mode instructions in 1.1 billion. It hid for ~230 M instructions
   because privileged writes are *accidentally* satisfied by `AP=0b10`; only an
   unprivileged access can expose it, and the first one the kernel makes is the
   `copyout` of `"/sbin/launchd"`. Fixing it reached
   `Process 1 exec of /sbin/launchd`. (`85c4653`)
8. **The ARMv6 CPUID registers, and `EBADARCH`.** That exec then failed with
   errno 86. All 385 ARM Mach-Os in the rootfs are cputype 12 / cpusubtype 6, so
   the disk was never wrong — the kernel's idea of its own CPU was.
   `do_cpuid()` reads MIDR, sees architecture field 0xF (which the ARM ARM
   defines as "described by the CPUID scheme, not by this field"), and goes on
   to read `ID_ISAR1` to check for Jazelle. We modelled CP15 c0 only for CRm==0
   and returned zero, so the check failed, the arch field stayed 0xF,
   `cpu_init()` indexed past its 7-entry table and stored `CPU_SUBTYPE_ARM_ALL`,
   and `grade_binary`'s `__switch8` (count byte 5, covering host subtypes 5..9)
   missed and returned grade 0 — `EBADARCH` for every armv6 binary on the disk.
   Fixed by returning the ARM1176JZF-S feature identification block for CP15 c0
   CRm 1 and 2 (ARM DDI 0301H §3.2); **no kernel patch, the kernel's logic was
   right and we were the ones not answering.** `ID_DFR0` deliberately stays 0
   where the real part says 0x33, because we have no CP14 debug unit and
   `do_debugid()` would take a non-zero value as licence to publish a breakpoint
   count; a test pins the two together. (`30a95d3`)
9. **A hardware SHA-1 engine we do not model, silently fabricating digests.**
   With the exec path open, launchd's first text page failed its code-signature
   hash and the thread spun `cs_invalid_page` → `psignal` without retiring a
   single user instruction. **The bytes were never wrong.** Two independent
   from-scratch verifications exonerated the image first — a UDIF verifier that
   decompressed all 7 `blkx` tables and checked every per-`blkx` CRC32 (zero bad
   entries, and the reconstruction is byte-identical to `rootfs_apm.img`), and an
   HFSX reader that walked the catalog and verified code-directory page hashes
   for every signed Mach-O on the volume (155 files, 6,731 code pages, 27.6 MB,
   zero mismatches, launchd 46/46 and dyld 56/56). The real cause:
   `SHA1UpdateUsePhysicalAddress` branches to a hardware engine for buffers of
   exactly 4096 bytes whenever `_performSHA1WithinKernelOnly` is non-NULL — a
   function pointer installed by `IOCryptoAcceleratorFamily`, which matched in
   our boot. `cs_validate_page` hashes exactly 4096 bytes, so it took the
   hardware path every time, read six words out of an unmodelled register file at
   0x38000000, and `SHA1Final` emitted that. **The clinching evidence was
   timing**: `SHA1Transform` costs ~2,262 Thumb instructions per 64-byte block, so
   4 KB should cost ~145,000 instructions; the observed `SHA1Init` → verdict
   interval was 14,329, an order of magnitude too few. Software SHA-1 provably
   never ran. Un-matching the `sha1` nub keeps the hook NULL; `-S` restores it.
   (`f01a9a4`)

### What M4 leaves behind, still unexplained

M4's criterion is met and then some, but two things are survivable rather than
understood, and that is the category that becomes a mystery three milestones
later.

- **The abort-site table saturates**, all data aborts with FSR 0x07 (page
  translation fault) on a marching sequence of kernel virtual addresses, in
  `IOBufferMemoryDescriptor::initWithPhysicalMask` and the kernel's own
  `_fleh_dataabt`. First at instruction ~116.6 M, DFAR 0xea110000. The kernel
  takes them and carries on; `_panic` is never reached. The table holds 48 entries
  and now **reports how many it dropped** rather than truncating silently — a
  silently truncated list reads as "these are all the abort sites", which is
  exactly the wrong thing to believe while diagnosing a wedge (`f01a9a4`).
- **22 distinct non-RAM physical pages** are now touched, up from 13, because far
  more drivers run. The unmodelled ones are the edge interrupt controller, the
  second VIC, GPIO, the clock/reset generator, i2c0/i2c1, spi0/spi1, the crypto
  block, and SDIO — where 10,003 of the 10,013 accesses are the CMD5 poll that
  correctly times out because no card is modelled. Every one is counted and
  attributed to a PC *and now to a kext*, which is the point; but each is a
  driver talking to a device that is not listening.
- **`AppleH1CLCD` does not start.** The display controller is
  `/device-tree/arm-io/clcd`, `compatible = "clcd,s5l8900x"`, physical
  0x38900000, interrupt 13. Nothing models it, so the kext never comes up — and
  it is the piece that turns a framebuffer into a display.
- **`AppleMerlotLCD` needs a panel ID.** `/device-tree/arm-io/spi0/lcd0` is
  `compatible = "lcd,merlot"` with `lcd-panel-id = 0x00000000`. Real iBoot reads
  the panel's ID over SPI and patches it in; we stand in for iBoot, so we have
  to write a non-zero one — the same in-place, same-length device-tree patch we
  already perform for the clock frequencies.
- **Three fault-path gaps, deliberately left unfixed** and recorded so they are
  not rediscovered as surprises (`e2d6c44`): `XN` is not checked on the
  instruction-fetch path, so executing an execute-never page raises no
  permission fault; unaligned accesses that cross a page boundary translate only
  the first page; and there is no external-abort source, so `DFSR.ExT` and
  status 8/c/e are unreachable. `DFSR[10]` and `DFSR[12]` are likewise never
  produced. The same audit *did* fix two real gaps: `CPSR.A` is now set on
  Prefetch Abort / Data Abort / IRQ / FIQ entry as the ARM ARM requires, and
  `CPS` is now correctly a no-op in User mode — honouring it was a privilege
  escalation that a kernel-only boot could never expose.

---

## 🔵 M5 — userspace → SpringBoard 🏆

**Criterion, in order, each independently observable:**

1. The kernel mounts a root filesystem and `launchd` executes its first
   instruction in user mode.
2. Daemons start, and the system log shows them doing it.
3. SpringBoard renders the home screen into the framebuffer.
4. A touch delivered from the host's screen moves something on the guest's.

**Where we actually are: criterion 1 is met, and criterion 2 has not started.**
The root filesystem mounts — the real 413 MB HFSX volume from the IPSW, as `md0` —
and `launchd` executes user-mode code and issues system calls. It then stops,
and it stops on *us*.

Measured with the real rootfs, from `bootkernel`'s milestone probes:

```
_load_init_program        first @ 230,864,582
_execve                   first @ 230,895,729
_mac_vnode_check_exec     first @ 230,968,564
_grade_binary             hits 3
_load_machfile            first @ 231,011,045
_ubc_cs_blob_add          hits 2
_cs_validate_page         hits 15         first @ 232,201,298
cs_validate:hashing       hits 15
cs_validate:bad_hash      NEVER REACHED
cs_validate:no_hash_exit  NEVER REACHED
_cs_invalid_page          NEVER REACHED
_psignal                  NEVER REACHED
_fleh_swi                 hits 24         first @ 233,031,366
_mach_msg_overwrite_trap  hits 12         first @ 233,347,392
_unix_syscall             hits  5         first @ 234,013,919
_fleh_undef               hits  1         first @ 234,731,379
_panic                    NEVER REACHED

stopped after 234,731,493 instructions: UNDEFINED INSTRUCTION
  encoding at pc: 0xecb10a20 (ARM)
  lr 0xc006ae0d (_vfp_trap+0x38)
```

Fifteen pages validate cleanly, no page is ever invalidated, twenty-four SWIs are
taken, twelve Mach traps and five BSD system calls are serviced. Then the kernel
takes an undefined-instruction trap into `_vfp_trap` — the handler whose job is to
emulate or execute the VFP instruction that faulted — and the encoding it reaches
is one **our interpreter does not implement**, so the machine halts at the
instruction rather than computing something plausible. That is M1's rule doing its
job: the emulator named the gap instead of corrupting state that would fail
somewhere else.

**This is progress, and it is not M5.** Five system calls is not a userland. No
daemon has started, nothing has been logged by userspace, and SpringBoard needs a
display controller and a panel that do not exist yet.

### The M5 work item list, as it now stands

- **VFP arithmetic in the interpreter.** This is the current wall, and it was
  predicted: `e2d6c44` listed the VFP undefined trap as a known gap left
  deliberately unfixed. It is now reachable, which is this project's usual shape.
- **Nothing symbolizes userspace.** Every diagnostic here resolves against the
  kernelcache. The instant launchd retires an instruction, the milestone table,
  the profiler and the kext symbolizer all go quiet — which is exactly the
  regime we have now entered. This is a known gap, not a solved one.
- **The display path** — `AppleH1CLCD` plus the Merlot panel ID, then the guest
  framebuffer surfaced through the app's Metal view.
- **Multitouch**, mapped from the host touchscreen to the guest's controller.
  `AppleMultitouchZ2SPI` already starts and reports "using DMA for bootloading",
  so the driver side is live and waiting for a device.
- **NAND VFL/FTL**, if we ever mount a real NAND image rather than a RAM disk.

### The two things that could have killed M5

Both were investigated in `docs/activation.md` and neither is now judged a
killer: guest code signing is **KILLED** (AMFI's exec-time check is switched off
by boot-args the kernel reads when `/chosen/debug-enabled` is set, and we are
iBoot), and activation is **MANAGEABLE** (an unactivated device still renders
SpringBoard, as its activation lock screen).

**One honest caveat, and one piece of corroboration.** The caveat: neither switch
has yet been *exercised* in a boot — the current recipe does not set
`debug-enabled` or `cs_enforcement_disable` — so "KILLED" remains a reading of the
kernel's code rather than a demonstration. The corroboration: code signing has
since been *survived without either switch*. `cs_validate_page` now runs fifteen
times over launchd's pages and validates every one, `cs_invalid_page` is never
reached, and pid 1 reaches user mode. Apple's own signatures verify against
Apple's own bytes on our emulated hardware, which is a stronger result than
switching the check off would have been.

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

**Foundation merged (J2, `9363283`), behind `-DIOS3VM_JIT=ON` and off by
default.** `core/src/jit/{a64_emit,jit_translate,jit_mem}.c`, 297 assertions.
The honest measure is that this is **roughly 15% of a JIT that could carry a
boot**:

- No code cache, no dispatcher, no chaining, no invalidation — so **nothing calls
  the translator yet**.
- A translated block currently costs *more* than interpreting it: mean run is 8.1
  instructions against a ~37-instruction prologue/epilogue.
- Thumb is declined entirely, which caps coverage at ~31% of retired
  instructions — Thumb is 68.95% of what kernel init actually executes.
- Emitted code has not been shown to execute correctly on this dev box, because
  it is x86. The `jit` CI job runs the execution test on Apple Silicon runners;
  elsewhere it reports `SKIP`, and a `SKIP` on a macOS runner would mean that
  coverage silently disappeared.

It was merged now to stop it rotting against a core that is moving fast, not
because it is nearly done.

**Observable:** SpringBoard at interactive frame rates. **Already delivered by
the same design work:** snapshot/restore (`95eaf8b`), which `docs/dynarec.md`
§11.2 identified — correctly — as the thing that actually fixes the M5 iteration
loop. Cold boot to 900 M instructions is 140 s; restore-at-200 M and continue is
34 s.

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
proves nothing about the paths you have not reached.** Every suite was green
throughout every bug above. What found them was running real firmware and
insisting the emulator complain loudly the moment it was asked for something it
did not have.

### Three lessons the last session added

**Systemic beats device-specific, and the two look identical from the log.** Most
wall-clearing here has been device whack-a-mole: model one more peripheral,
un-match one more driver, patch one more wait. Each buys one symptom. Two fixes
were a different kind — `TTBCR.N`/`TTBR1` and `DFSR.WnR` were each a single
architectural gap in the CPU itself, and each unblocked a dozen symptoms at once
(the first started the entire IOKit driver tree; the second reached userspace).
Both *presented* exactly as another device problem: a spin in an unsymbolized
kext. So the question worth asking before modelling the next peripheral is
**"could one thing the CPU gets wrong explain all of these at once?"**

**A bug invisible for 200 M+ instructions is almost always reachable only from an
unprivileged or otherwise rare path.** `DFSR.WnR` hid for ~230 M instructions
because privileged writes are accidentally satisfied by the `AP=0b10` the kernel
repairs the PTE to; only `STRT`/`LDRT` or real user mode could expose it. `CPS`
being honoured in User mode is unreachable from a kernel-only boot by
construction. When something has been silently wrong for a very long time, do not
look for a rare *value* — look for a rare *mode*.

**"The profile blames one unsymbolized kext" is now a solved problem.** It cost
five separate diagnosis cycles (ADMFMC, MBX, IORTC, the TTBR abort storm, the
post-SDIO stall) before anyone fixed the tool instead of the symptom. The kext
symbolizer (`f105360`) maps `__PRELINK_TEXT` to bundle identifiers out of
`__PRELINK_INFO`, so an address now resolves to `<bundle-id>+0xNNNN` and the
report gained "time by prelinked kext" and "hottest individual PCs". Per-kext
*function* names are impossible, not merely unimplemented — the kernelcache
builder strips each kext's `LC_SYMTAB` — which is exactly why the hottest-PC list
exists. The whole procedure these tools add up to is written down in
[debugging.md](debugging.md), so it does not have to be rediscovered a sixth
time.

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

# Roadmap

The guiding rule: **every milestone ends in something observable.** Not "the MMU
is implemented" but "the kernel builds its own page tables, enables the MMU, and
keeps running". Not "drivers work" but "here are the bytes Apple's driver
printed over our UART". If a milestone cannot be phrased as a thing you can
watch happen, it is not a milestone, it is a hope.

Measurements below come from named real-firmware runs and are historical unless
explicitly identified as current. Targets and designs are labelled separately;
the absence of a newer private-firmware trace must not be read as proof that the
old stopping point still applies.

Honest positioning: to the maintainers' knowledge, **no publicly documented
open-source emulator has booted iPhone OS 3.x to SpringBoard.** The closest prior
art found in the project's survey (devos50/qemu-ios) reaches SpringBoard on 1.1
and 2.1.1 using emulated iPod touch hardware. This is project positioning, not a
proof that no private or unindexed implementation exists.

---

## Status at a glance

| | Milestone | Observable completion criterion | State |
|---|---|---|---|
| **M0** | Pipeline online | Core/JIT/test workflows and iOS packaging are configured; the app has historically run a core self-test on the phone | ✅ done; workflow logs are the current CI verdict |
| **M1** | ARMv6 interpreter | ARM + Thumb + VFPv2 on the reached path; unimplemented encodings trap instead of guessing | ✅ done for the reached boot path, including ARM1176 WFI and the reached ARMv5TE DSP multiply families; not every architectural extension is implemented |
| **M2** | SoC bring-up | A bare-metal payload prints over the emulated UART; a timer IRQ is taken and returned from | ✅ done and covered by host tests |
| **M3** | Firmware containers + LLB execution | Real IMG3s parse and decrypt; an extracted real Apple LLB payload executes; the kernelcache is extracted | ✅ done; SecureROM/iBoot execution remains future full-chain work |
| **M4** | XNU boots and logs | The kernel reaches `bsd_init`, prints, and Apple's own kexts match and start | ✅ **done** — plus the real root filesystem mounts |
| **M5** | Userspace → SpringBoard | `launchd` runs; the home screen renders and takes a tap | 🔵 **in progress.** The historical direct-RAM chain reached a clean 2.98 B cap. The fresh 128 MiB external-md run07 path retains `launchd`, fsck, the `/dev/md0` root mount, `mDNSResponder`, and `systemShutdown false`, then exits cleanly at 2 B with a 50.71 MiB free-page low. Its framebuffer was disabled and CLCD status/mask/scanning were zero, so there is no SpringBoard, framebuffer, or tap proof. The app is still a demo host. |
| **D** | Dynarec (parallel) | SpringBoard at interactive frame rates on the phone | 🔵 emitter + ARM/Thumb translator and host execution tests exist (off by default); no code cache or dispatcher calls them |
| **N** | Guest networking (parallel) | The guest resolves a name and fetches a URL | ⚪ designed, not built |
| **A** | Guest audio (first-device track) | Guest PCM reaches the host speaker without blocking the CPU thread | ⚪ priority, not designed or built |

CI builds the portable core on Linux, macOS and Windows, runs strict-warning and
sanitizer jobs, and executes emitted JIT blocks on the arm64 macOS runners. The
iOS workflow proves compile, link, fake-sign and packaging only; it is not an
on-device runtime or real-firmware boot test. Exact assertion totals change with
the suite and optional private firmware, so the workflow log is authoritative.
At `df9dc7b`, `core-tests` run
[`30004015881`](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30004015881)
and `ios-build` run
[`30004015807`](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30004015807)
both completed successfully with the faultable raw bridge.

---

## ✅ M0 — Pipeline online

**Criterion:** CI green, and an `.ipa` that runs real ARM code through our core on
the device.

- Portable C11 core builds and unit-tests locally (MinGW/GCC) and in CI.
- The iOS workflow builds on a macOS runner, fake-signs with `ldid`, and packages
  an `.ipa` artifact — no Apple Developer account is in the CI loop. CI does not
  install or launch that artifact.
- The app's on-device self-test runs ARM instructions through the interpreter and
  drives the emulated timer → VIC → CPU interrupt path. It also reports
  `CS_DEBUGGED` and RWX-mapping preflight hints, but deliberately does not branch
  into generated code during startup. The JIT execution verdict remains an
  unrecorded, opt-in device check.

**Historical device observation:** `r0 = 42`, computed by the interpreter.

---

## ✅ M1 — ARMv6 interpreter

**Criterion:** an ARM1176 interpreter broad enough for the boot path, in which
every unimplemented encoding *traps* rather than executing something plausible.

Done for every path reached so far, and validated by historical long Apple-code
runs: base ARM and Thumb data processing, branches/interworking, the implemented
single and block-transfer forms, halfword/sign-extending loads, long multiplies,
banked registers and mode switching, the reached exception entry/return paths,
the required CP15 subset, selected ARMv6 extend/reverse media operations, the
ARMv6K exclusive family, an ARM1176 VFPv2 module, and all five related ARMv5TE
signed-DSP multiply families (`SMULxy`, `SMLAxy`, `SMLALxy`, `SMULWy`, and
`SMLAWy`). The ARM1176 CP15 wait-for-interrupt form used by XNU also advances
devices directly to the next wake edge without fabricating retired instructions.
This is reached-path coverage, not architectural completeness: LDRD/STRD, other
DSP/media families, the CP14 debug unit and other documented forms still trap
deliberately.

**The rule that makes this milestone worth anything:** an encoding we have not
implemented returns `ARM_UNDEFINED` and stops the machine. It never falls
through to "close enough". CP15 is the one documented exception — unmodelled
config registers read as zero, because kernels probe that space far too widely
to trap on it.

**Observed:** the CPU suite is green in the reviewed CI run. Its assertion count
is intentionally not frozen here; VFP and edge-case coverage continue to grow.

---

## ✅ M2 — SoC bring-up and UART

**Criterion:** guest code running on the emulated S5L8900 prints text, and an
interrupt is delivered and returned from.

- ARMv6 short-descriptor translation and permission behaviour sufficient for the
  reached path: 1 MB sections, supersections, coarse tables with 64 KB large and
  4 KB small pages, domain/AP/XN checks, and data/prefetch aborts wired into
  execution (DFSR/DFAR, IFSR/IFAR). Page tables are walked out of guest RAM
  through the normal bus. This does not claim every memory attribute or external
  abort source is modelled.
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

The machine suite is green in the reviewed CI run; use its log for the current
assertion count.

---

## ✅ M3 — Firmware containers and LLB execution

**Criterion:** real, unmodified Apple firmware out of a real IPSW parses and
decrypts, and an extracted real LLB payload executes in the firmware harness.
This completed milestone is narrower than the full secure-boot chain:
SecureROM is not modelled, iBoot itself has not executed, and SHSH/CERT presence
is recorded without RSA verification. `bootkernel` currently synthesizes an
iBoot-like handoff into XNU. Full SecureROM → iBoot execution remains future
work rather than evidence claimed by M3.

- IMG3 container parser — every tag present in genuine 7E18 firmware
  (`TYPE`/`DATA`/`VERS`/`SEPO`/`BORD`/`KBAG`/`SHSH`/`CERT`) is one we handle.
  Bounds-checked in 64-bit arithmetic, because this is the first component to
  touch a user-supplied file.
- A self-contained AES-128/192/256 validated against the FIPS-197 known-answer
  vectors, plus LZSS for the kernelcache. No OpenSSL; the core keeps its
  zero-dependency property.
- Apple's own device-tree format (not FDT): node and property traversal, path
  lookup, depth-limited and bounds-checked against malicious input.
- Integrated NOR flash with IMG3 scanning, writable and persistent (the shape an
  untethered jailbreak needs). A standalone raw-NAND/storage substrate models
  erased/programmed bit behaviour and persistence in host tests, but it is not
  connected to the S5L8900 machine bus or boot path.

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
decrypted from a stock IPSW, with the real 413 MiB (433,274,880-byte) root
filesystem attached as a RAM disk. This was measured at `9363283`. A later
historical run made pid 1 progress instead of spinning and halted at instruction
234,731,493 on the VFP encoding recorded under M5; that encoding is implemented
now, so neither count states the current stopping point.

| Measurement | Value |
|---|---|
| `_panic` / `_Debugger` reached | **never** |
| `_bsd_init` reached | instruction 64,567,734 |
| Root filesystem | **`BSD root: md0, major 2, minor 0`** |
| Console output | **4,595 bytes** |
| Distinct functions executed (sampled) | 1,024 (the profiler's table is now the limit, and says so) |
| `_DTGetProperty` calls | 858 — IOKit walking our device tree |
| FIQ entries / cost | 385 / 38,235 instructions (0.0% of the run) |
| `STREX` executed / failed | 2,715,561 / 13 — all retries were observed in `lck_mtx_*`; the trace does not establish their cause or interleaving |
| Exception returns into Thumb | 351, of which 204 land 4-byte aligned (~58% in this recorded run; no hardware distribution is assumed) |
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
memory we handed it. It proves console rendering, not that Apple's display
driver started; the current tree has a tested CLCD model, but the run below did
not reach that kext's code.

The current CLCD correction does not change that historical result. It prepares
the controller handoff for a new run by treating `0x0d8..0x0ec` as window
configuration rather than panel timing, seeding the actual `VIDTCON0..3`
registers at `0x20c..0x218` with iBoot-compatible N82 320x480 timing, and
requiring every hardware scanout gate before frames or wake edges are produced.
No real-firmware display run has validated that preparation yet.

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
   resumed at a 4-byte-aligned address rather than producing the mixed alignment
   expected from the observed return targets, and 372 of them (48.9%) needed a
   +2 correction. Two competing
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
5. **512 MiB is the current hard ceiling.** A historical run with `-R 768`
   panicked at ~34 M in early VM init with a null-zone dereference.
   `arm_vm_init` hardcodes `virtual_avail = 0xe0000000`, so at the documented
   virtual base the kernel's physical-linear window is exactly 512 MiB;
   advertising more makes `zone_virtual_addr` index a `pv_head_table` that is
   still zero during `zone_bootstrap`. Current source also rejects any RAM
   aperture that overlaps NOR at `0x28000000`, which is exactly 512 MiB above
   the `0x08000000` SDRAM base. Real S5L8900 devices shipped ≤256 MiB, so
   hardware never reached this oversized path.
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
   lets a much broader driver set run: timers, I²C, I²S, SPI, USB PHY, twelve
   DMA channels, uart0/1/3/4, the spi-baseband mux, `AppleMultitouchZ2SPI`,
   `AppleMobileFileIntegrity`, `ApplePCF50635PMU`. Tests cover the N=0 regression
   guard, the N=2 geometry, N=1/N=3 to prove the formulas scale, that
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
   private, untracked historical verifications exonerated the image first — a
   UDIF verifier that
   decompressed all 7 `blkx` tables and checked every per-`blkx` CRC32 (zero bad
   entries, and the reconstruction is byte-identical to `rootfs_apm.img`), and an
   HFSX reader that walked the catalog and reported code-directory page hashes
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
   (`f01a9a4`). Those verifier tools and outputs are not present in the public
   tree, so this evidence is recorded history rather than a reproducible current
   check.

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
  more drivers run. The unmodelled ones include the edge interrupt controller,
  GPIO, the clock/reset generator, i2c0/i2c1, spi0/spi1, the crypto
  block, and SDIO — where 10,003 of the 10,013 accesses are the CMD5 poll that
  correctly times out because no card is modelled. Every one is counted and
  attributed to a PC *and now to a kext*, which is the point; but each is a
  driver talking to a device that is not listening.
- **`AppleH1CLCD` was not observed starting in this run** — but NOT because the
  CLCD was unmodelled. That earlier claim was wrong on both halves:
  `core/src/soc/clcd.c` is a tested model, and the nub's registers were never
  read. The sampled profiler also recorded no PC in the kext, but its
  one-in-1,024 sampling interval cannot prove that the kext executed literally
  zero instructions. The display controller is
  `/device-tree/arm-io/clcd`, `compatible = "clcd,s5l8900x"`, physical
  0x38900000, interrupt 13.
- **`AppleMerlotLCD` needed a panel ID in this run.**
  `/device-tree/arm-io/spi0/lcd0` was
  `compatible = "lcd,merlot"` with `lcd-panel-id = 0x00000000`. Real iBoot reads
  the panel's ID over SPI. The current CLI can patch a non-zero value, seed CLCD
  window 0 and capture the active buffer, but there is no fresh private-firmware
  trace proving how far the Apple display kexts proceed with those changes.
- **The CLCD seed needed real timing, not mislabeled window words.**
  Offsets `0x0d8..0x0ec` are per-window auxiliary configuration pairs; actual
  `VIDTCON0..3` lives at `0x20c..0x218`. The corrected N82 handoff seeds
  `VIDCON0 = 0x441` for the 54 MHz clock divided by five with scanout enabled,
  plus `VIDCON1 = 0x8` for inverted VCLK. The porch/sync values are fixed for
  N82, while `VIDTCON2` derives from the requested geometry; production 320x480
  yields `0x013f01df`. Initial `0x0d8`, `0x0e0`, and `0x0e8` window words are
  `0x1000`. Live scanout additionally requires start state, `CLCD_CTRL` global
  enable, and `VIDCON0` bit 0. This is host-side correctness preparation, not
  evidence that either Apple display kext ran or that SpringBoard rendered.
- **The same audit recorded three fault-path gaps** (`e2d6c44`). Two have since
  been closed and regression-tested: instruction fetches enforce `XN`, and
  unaligned accesses that cross a page boundary translate both pages. The third
  remains: there is no external-abort source, so `DFSR.ExT`, status 8/c/e,
  `DFSR[10]` and `DFSR[12]` are not produced. The audit also fixed two real gaps:
  `CPSR.A` is now set on
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

**Last demonstrated boundary:** criterion 1 is met and criterion 2 is partially
observable in the CLI harness. The real HFSX root filesystem mounted as `md0`,
`launchd` executed user-mode code, and `mDNSResponder` ran as pid 14. A current
checkpoint chain restored at 2.2 B retired instructions, crossed the former
`SMULBB` stop and wrote a 2.4 B checkpoint. The 2.4 B → 2.8 B interval wrote a
2.7 B checkpoint, observed one new `_execve` first at 2,605,595,575, and ended
with `systemShutdown false`. Restoring 2.7 B wrote a 2.85 B checkpoint and
reached the configured 2.9 B cap. None of those intervals reached `_panic`,
`Debugger`, or an emulator undefined-instruction stop. A diagnostic continuation
from 2.85 B then reached 2,944,340,624 instructions and stopped on `0xe6cf3073`,
ARMv6 `UXTB16 r3, r3`, in user mode. After the complete paired-extend family was
implemented and tested, replaying that same checkpoint cleared the instruction,
wrote a 2.97 B checkpoint and reached the configured 2.98 B cap with status
`OK`. The interval recorded two `_load_machfile` paths, 400 code-page
validations, 4,266 software-interrupt entries and 3,373 Unix syscalls. No log or
framebuffer capture from these runs proves SpringBoard started.

That diagnostic also crossed the free-page target without an immediate OOM:
the pool fell from 317 pages at 2.9 B to a low of 97 pages at instruction
2,934,505,472, recovered to 253 pages at the former opcode stop, and ended at
214 pages at 2.98 B, against a target of 250. The
roughly 445 MiB pinned RAM disk remains a severe device-memory constraint. The
completed audit's writable md bulk-copy design is now integrated as a guarded
cold-boot mode. A fresh 128 MiB real-firmware continuation reached `launchd`,
fsck, and the first raw `/dev/rmd0` read at 402,741,536 instructions with 21,187
free pages (82.76 MiB), 6,715 successful strategy reads, and no bridge failure.
That pre-raw-bridge run stopped intentionally at `_mdevrw`. Run03 crossed the
guard and reached its 420,000,000 cap, but fsck exited with signal 8. Run04
reproduced two exact causes by 405 M: the segment-5 offset-zero 32 KiB read
needed native write-side demand paging at user VA `0x01001000` (`FSR 0x807`),
and a 32 KiB read at `0x1bd30000` crossed media end `0x1bd33000` by 20 KiB.
Both recurred across fsck's `-p` and `-fy` passes.

Run05 cleared both request shapes in a fresh cold boot. It reached the
430,000,000-instruction cap with exit status 0 after `launchd`, `Running fsck
on the boot volume...`, and `/dev/md0 on / (hfs, local, noatime)`. The raw path
recorded two reads, two native redirects, two checked completions, zero guest
errors, and zero pending continuations. Of their 65,536 bytes, 45,056 came from
the media and 20,480 from the coherent guard. The aggregate external path completed
6,901 reads (28,295,168 bytes) and one 512-byte write with zero failures; 6,899
reads and that write used the strategy bridge. The work image remained
466,825,216 bytes, and the lowest free-page sample was 20,820 pages (81.33 MiB)
at instruction 425,852,928. `_execve` remained at 11 hits and `_load_machfile`
at 6. It has still not produced a SpringBoard framebuffer.

Run06 extended that same fresh-cold architecture to 1,000,000,000 instructions
with exit status 0 and empty stderr. The serial log retained the
launchd/fsck/root-mount sequence, added both `mDNSResponder[14]` Seatbelt lines,
and ended with `systemShutdown false`. It completed 10,004 external reads
(40,994,304 bytes), 27 writes (107,008 bytes), and zero failures; strategy
accounted for 10,002 reads and all 27 writes. The raw path remained two reads,
two native redirects, two checked completions, zero guest errors, and zero
pending continuations, with 45,056 media bytes plus 20,480 coherent-guard bytes.
The low-water sample was 17,221 pages (67.27 MiB) at instruction 980,615,168.
The work image remained 466,825,216 bytes and the source firmware hashes were
unchanged. `_execve` remained at 11 hits while `_load_machfile` advanced to 25.
No SpringBoard frame was captured.

Run07 extended the fresh 128 MiB external-md cold path to 2,000,000,000
instructions with exit status 0. Its 234,838-byte stdout retained launchd,
fsck, `/dev/md0` root mount, both `mDNSResponder[14]` Seatbelt lines, and
`systemShutdown false`; stderr was empty. The final PC was `0x3145ad4c` in USR
mode (`CPSR 0x20000010`), and 731,259,769 instructions (36.6%) retired in USR
mode. `_execve` reached 12, `_load_machfile` 32,
`_thread_bootstrap_return` 92,620, and `_unix_syscall` 58,166.

The external bridge completed 12,782 reads (52,372,992 bytes), 82 writes
(325,120 bytes), and zero failures; strategy handled 12,780 reads and all
writes. Raw I/O remained two reads, no writes, no guest errors, two native
redirects, two completions, and zero pending continuations. It read 45,056
media bytes plus 20,480 coherent-guard bytes and wrote neither region. The run
ended with 13,000 free pages (50.78 MiB); its low was 12,983 pages (50.71 MiB)
at instruction 1,836,056,576. The work image remained exactly 466,825,216 bytes
and the kernel, device-tree, and rootfs source hashes were unchanged.

Run07 deliberately had the framebuffer disabled. CLCD status, interrupt mask,
and scanning were all zero. It therefore provides no evidence that SpringBoard
started and no validation of the real display path.

For chronology, this is the much earlier pre-VFP measurement from
`bootkernel`'s milestone probes:

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

Fifteen pages validated cleanly, no page was invalidated, twenty-four SWIs were
taken, and twelve Mach traps and five BSD system calls were serviced. That
historical run then stopped on VFP.

XNU does not leave VFP enabled. `_init_vfp` grants CP10/CP11 full access once, and
from then on the gate is `FPEXC.EN` alone, cleared per thread — so **the first VFP
instruction a thread executes is supposed to take an Undefined exception**, which
the kernel handles by enabling VFP and re-running it. `d021205` made us vector
exactly those to the guest, using `_sleh_undef`'s own six encoding masks as the
discriminator so that a genuinely unimplemented encoding still names itself rather
than being swallowed by the guest's handler. That path now works and has been
crossed by the current run.

What halted that machine was the *next* instruction along it: `0xecb10a20`,
`VLDMIA r1!, {s0-s31}` — the load-multiple by which `_vfp_switch` restores a
thread's VFP register file. The interpreter correctly stopped instead of
guessing. `core/src/arm/vfp.c` now implements that family and its regression
tests; this trace remains evidence for why the implementation was needed, not a
current blocker report.

The next exact user-mode stop was `0xe1630381` at VA `0x33dba604`, decoded as
`SMULBB r3, r1, r3`. Implementing only that literal would have hidden adjacent
failures, so the interpreter now implements and tests the complete related
ARMv5TE set: `SMULxy`, `SMLAxy`, `SMLALxy`, `SMULWy`, and `SMLAWy`, including
halfword selection, signed truncation, accumulator overflow/Q behavior, aliases,
conditions and invalid-register cases. Restoring the 2.2 B checkpoint with that
implementation cleared the instruction; chained restores then reached the 2.9 B
cap normally. The next fail-closed user-mode stop was `0xe6cf3073`, ARMv6
`UXTB16 r3, r3`, at instruction 2,944,340,624. The complete paired-extend
family now clears that stop, and the same snapshot reaches the 2.98 B cap.

**This is progress, and it is not M5.** A live daemon and a cap-limited run are
not SpringBoard. The core now has a CLCD model and panel seed, but the iOS app
still runs only a synthetic guest and has no touch or audio path.

### The M5 work item list, as it now stands

- ~~**VFP load/store and arithmetic in the interpreter.**~~ **DONE and covered
  by the VFP suite.** Current real firmware has crossed the old `VLDMIA` wall.
- ~~**ARM1176 idle handling.**~~ **DONE and covered by CPU/machine tests.** XNU's
  CP15 WFI advances timer/CLCD state to the next enabled VIC wake edge without
  counting made-up CPU instructions; a current real-guest run exercised it.
- ~~**ARMv5TE signed DSP multiplies.**~~ **DONE for the complete related family:**
  `SMULxy`, `SMLAxy`, `SMLALxy`, `SMULWy`, and `SMLAWy`. The exact current-path
  stop `0xe1630381` was cleared. The separately forced dirty-volume `fsck_hfs`
  scenario still needs a new end-to-end run; implementation coverage is not a
  claim that the full check has completed.
- ~~**ARMv6 paired extend instructions.**~~ **DONE for the complete paired
  family.** `SXTB16`, `SXTAB16`, `UXTB16` and `UXTAB16` implement all four
  rotations and independent lane wrap, with alias, condition, flag-preservation,
  reserved-bit and invalid-register tests. The exact `0xe6cf3073` stop was
  replay-cleared to a clean 2.98 B cap. Adjacent `REV*` PC-operand gaps were
  hardened at the same time.
- **A shared, memory-bounded guest session.** Move real-boot orchestration out of
  the CLI into portable C used by both `bootkernel` and the app. The bounded
  primitives now exist: `vm_source` provides portable ranged reads and
  `bootkernel` validates the complete layout before direct-streaming the rootfs
  into final guest RAM. The app does not use them yet; it still needs a shared
  session, user-owned file selection and explicit errors.
- **Nothing symbolizes userspace.** Every diagnostic here resolves against the
  kernelcache. The instant launchd retires an instruction, the milestone table,
  the profiler and the kext symbolizer all go quiet — which is exactly the
  regime we have now entered. This is a known gap, not a solved one.
- **The display path** — the CLCD model now separates `VIDTCON0..3` timing from
  the `0x0d8..0x0ec` window configuration, seeds an iBoot-compatible N82
  handoff, and gates frame publication and WFI edges on genuinely live scanout.
  The app's CoreGraphics bridge follows a validated active window only while
  those gates are live. This still needs a display-enabled real-firmware run and
  the shared real-guest session; Metal is optional and not implemented.
- **Multitouch**, mapped from the host touchscreen to the guest's controller.
  `AppleMultitouchZ2SPI` already starts and reports "using DMA for bootloading",
  which proves that the recorded boot reached that request. Device, DMA and
  input-report semantics remain unimplemented and unproven.
- ~~**Free space on the root volume.**~~ **DONE** — `bootkernel --grow <MB>`
  (default 32) grows the HFS+ volume in the loaded copy of the RAM disk;
  `firmware/rootfs.img` is untouched. TN1150 layout, four edits, validated
  before and after and refusing loudly on anything unexpected; see BOOTLOG
  "The volume had zero free blocks" for the detail and the numbers. Two things
  came out of it that are *not* done:
  - **The deliberately forced full `fsck_hfs -fy` check still needs separate
    revalidation.** The historical
    forced scan stopped on `SMULBB r6, r3, r5` at `fsck_hfs+0x12130`. The full
    related multiply family is implemented now, so that exact CPU gap is closed.
    Run04 entered both the `-p` and `-fy` paths, but each reproduced the raw
    demand-page/tail-read pair before fsck could establish a filesystem result.
    Run05 cleared that pair and the normal cold-boot fsck path progressed to
    `/dev/md0 on / (hfs, local, noatime)`; because it no longer needed the
    fallback, that run does not by itself exercise a deliberately forced full
    `-fy` scan.
  - **In the historical comparison it changed nothing by itself.** Out to 3 G
    instructions the console was
    identical with and without `--grow`. And `_execve` stuck at 11 was never
    the daemon counter it looked like: launchd spawns jobs with `posix_spawn`,
    which that probe does not see, and `mDNSResponder[14]` is running in both
    runs. Free space was not what was holding the LaunchDaemons — and neither
    was `execve`.
- **Recover pinned RAM without inventing an invalid physical map.** Direct
  streaming removed the second host-side rootfs copy, but the RAM disk is still
  static memory below `topOfKernelData`, so every mebibyte of volume is a
  mebibyte off the guest's free page pool: 58.93 MiB at the documented `-R 512`
  with `--grow 32` before later userspace consumption. A continuation beyond
  2.9 B fell to 97 pages (0.38 MiB), recovered to 253 pages at the former opcode
  stop and ended at 214 pages at 2.98 B against a 250-page target. Reclamation
  therefore appears active,
  but such headroom is still unsafe for an iOS host and the roughly 445 MiB
  pinned disk remains a major architecture frontier. The audit has ruled out a
  simple external PA aperture: `_bcopy_phys` converts both operands through one
  fixed DRAM direct-map delta. The portable bounded writable-block API, locked
  descriptor file adapter, privileged-only transactional SVC seam, and the
  writable range- and page-gated md-strategy bulk-copy bridge are now
  implemented and tested. An exact 7E18 manifest gates the complete decrypted
  kernel image, parsed ARMv6 Mach-O layout, fixed mapping, and all five expected
  patch sites before an atomic write. A bounded generic
  work-image provisioner now copies the immutable HFS source, validates reserved
  and allocation metadata, rewrites the unique stock fstab, grows/revalidates,
  flushes and publishes without replacement. `bootkernel --external-md` now
  exact-gates the supported kernel, device tree, and rootfs; publishes the md0
  media token outside fixed 128 MiB DRAM; and installs strategy plus raw-uio
  bridges after setup. A measured pre-raw-bridge cold run reached the first
  32 KiB `/dev/rmd0` read at 402,741,536 instructions after 6,715 strategy reads
  (27,479,552 bytes), zero writes, zero bridge failures, and with 82.76 MiB of
  guest pages still free. Run03 crossed that guard and reached 420 M, but fsck
  exited with signal 8. Run04 at 405 M reduced the failure to two repeatable
  contracts: native write-side demand paging at user VA `0x01001000`
  (`FSR 0x807`) and a 32 KiB read with 12 KiB inside the media plus 20 KiB in
  the adjacent allocation tail. The closest public XNU `_mdevrw` calls
  `uiomove64` once without a logical EOF check.

  The implemented replacement exact-patches `_mdevrw` to
  `svc #0xe3; svc #0xe4`, adds the explicit `ARM_SVC_REDIRECTED` control-flow
  result, and redirects faultable requests through exact Thumb `_uiomove64`
  at `0xc0128d14`. Four 128 KiB guest bounce slots, keyed by entry kernel SP,
  are reserved below `topOfKernelData`; the completion SVC validates and
  releases the pending slot. A zero-initialized, coherent 128 KiB in-memory
  tail preserves later reads without extending the immutable source or work
  image. Fresh run05 validated that design against the real firmware: two raw
  reads used two native redirects and two completions, with zero raw guest
  errors, zero pending continuations, and zero external failures. Run06 retained
  those results to a clean 1 B cap. Run07 then retained them to a clean 2 B cap,
  after fsck, the `/dev/md0` root mount, `mDNSResponder` Seatbelt setup, and
  `systemShutdown false`. Across the latest run the external path completed
  12,782 reads and 82 writes with zero failures, while the work image retained
  its exact 466,825,216-byte length and the firmware hashes remained unchanged.
  The latest serial run disabled the framebuffer and had zero CLCD
  status/mask/scanning, so it is not display evidence.
  Snapshot backing identity/overlay state follows, and global `_bcopy_phys`
  replacement remains forbidden. Historical
  older-source experiments reported 312 MiB and
  248 MiB pools with `-R 768 -Y`, but neither reached `_load_init_program` and
  current source correctly rejects both configurations because their RAM
  apertures overlap NOR at `0x28000000`. Making `-Y` useful now requires a
  deliberate physical-map design as well as resolving `gVirtBase` below the
  kernel's compiled-in `VM_MIN_KERNEL_ADDRESS`; the old 768 MiB commands are
  evidence, not valid current recipes.
- **NAND VFL/FTL**, if we ever mount a real NAND image rather than a RAM disk.
  This is the only route to a genuine `disk0`, and it is a large one. Both
  layers read undocumented Apple on-media formats: `AppleNANDFTL`'s FTL/VFL
  metadata, and the partition table that **`IOFlashPartitionScheme`** validates
  by magic and major version. (There is no `AppleAPM`/`AppleGPT`/`AppleFDisk`
  kext in this kernelcache — `IOFlashPartitionScheme` is what makes `disk0s1`
  and `disk0s2`, and it fails its probe outright unless its provider carries a
  `boot-from-nand` property.) Per the project rule we do not invent either
  format, so this stays parked rather than half-built.

### The two things that could have killed M5

Both were investigated in `docs/activation.md` and neither is currently judged
a blocker. Stock-signed `launchd` has passed fifteen page validations and reached
user mode without the enforcement-disable switches. The separate claim that
AMFI can be disabled through iBoot-style handoff properties and boot arguments is
confirmed by kernel disassembly but has **not** been exercised in a boot.
Activation remains a **plausibly manageable, unexercised risk**: an unactivated
device is expected to render SpringBoard's activation UI, while the exact 3.1.3
`lockdownd` data path remains to be verified against the binary. That could prove
a SpringBoard frame, but the home-screen criterion still needs an activation
route.

**Performance is a quantified desktop limitation and an unmeasured device
risk.** The interpreter ran a tight synthetic loop at roughly 14 million
instructions per second on the development host; real code with MMU translation
was slower still. No equivalent A9 result exists, so the desktop data establishes
that the current interpreter is far below the guest model's nominal rate, not
that SpringBoard will render or that the phone has a particular multiplier.

---

## ⚪ Parallel tracks

### Dynarec

An ARMv6→ARM64 JIT emitting into executable pages, with the interpreter as its
differential oracle. The backends have separate emitted and C semantics in
places. Current focused tests run short blocks through both engines and compare
their final architectural state and touched memory; a full per-instruction boot
differential harness remains planned.
The A9 host is chosen partly because it predates APRR (A11) and PAC/PPL (A12).
Whether the intended jailbroken
iPhone grants executable memory is still an on-device validation item, not a
portable-core assumption.

**Foundation present behind `-DIOS3VM_JIT=ON`, and off by default.** The
AArch64 emitter and ARM/Thumb translator have structural tests, and emitted
blocks run in the macOS arm64 CI jobs. A historical private, untracked
translation-eligibility sample improved substantially after Thumb support, but
that result is not reproducible from the public tree and is not boot coverage:

- There is no code cache, dispatcher, chaining or invalidation, so
  `s5l8900_run()` executes **zero** translated blocks.
- Unsupported forms are deliberately declined; the future dispatcher must
  synchronise state and run them through `arm_step()`. That boundary remains
  performance-critical correctness work.
- The iOS target excludes `core/src/jit/**`. Startup only reports
  `CS_DEBUGGED` and RWX mapping; it does not execute generated code. A future
  opt-in diagnostic checks that host precondition, not the emulator's translator
  or run loop, and no execution result from the target iPhone is recorded here.

The iPhone 6s Plus is the first optimization and validation target. The
translator stays in portable core code while executable-memory, cache-flush and
thread-policy details remain host adapters.

**Observable:** SpringBoard at interactive frame rates. Snapshot/restore
(`95eaf8b`) has already reduced one historical desktop replay from 140 seconds
for a cold 900 M-instruction run to 34 seconds when restoring at 200 M. That
reduces a known cold-replay cost; the current 2.2 B → 2.98 B checkpoint chain
also proves that restore works across the post-VFP userspace frontier, can
isolate a later opcode, and can replay through its fix. It does not
measure the phone's iteration loop or make the inactive JIT a boot prerequisite.

### Guest networking

Designed in [networking.md](networking.md), not built. The selected first route
is **PPP over emulated UART3**: the plan is for the stock guest `pppd` and
`pppserial` to turn the UART byte stream into `ppp0`, while a host-neutral
PPP/IP/NAT core exits through ordinary socket adapters. It needs no guest kext, emulator-specific tweak,
`utun`, raw socket or phone-wide routing change. The alternative paravirtual
Ethernet kext and real Marvell Wi-Fi model remain deferred routes, not the
recommended implementation.

N0–N2 (NAT, UART transport and PPP negotiation) can be built and tested with
small host fixtures. N3 needs the shared real-guest session and a reliable
`launchd` job path before device claims are meaningful. The CPU thread must
never block on network I/O; queues and protocol state stay portable while
`kqueue`/BSD sockets are one host adapter.

**Observable:** the guest resolves a hostname and fetches a URL over plain HTTP.

### Guest audio — first-device priority

Audio is now a first-device track for the iPhone 6s Plus, but **no guest audio
device or host sink exists today**. Start by proving which I2S/controller and
codec driver path the 3.1.3 kernel expects; do not invent register behavior to
make sound appear. The eventual device model publishes PCM through a bounded
queue. A host adapter performs format conversion and playback outside the CPU
thread; underruns become counted silence and overruns are bounded and counted.
The guest-facing model and queue contract stay platform-neutral even though the
first playback adapter will use iOS audio APIs.

**Observable:** a deterministic guest tone reaches the speaker, survives pause
and route changes without deadlock, and reports bounded underflow/overflow
counters.

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
(the first unblocked a broad set of IOKit drivers observed in that run; the
second reached userspace).
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
- **Wi-Fi through the real Marvell 88W8686** and GPU acceleration. "Route A"
  for networking — emulating the real NIC so Apple's driver binds unmodified —
  has documented SDIO/controller reconnaissance in `networking.md`, but the
  Marvell firmware protocol remains underspecified and the route is deliberately
  deferred. Guest audio is now in scope for the first device, but remains
  unimplemented.
- **App Store distribution.** Out of scope. The current ad-hoc signing,
  jailbreak-dependent executable-memory policy and user-supplied firmware plan
  are not an App Store distribution path.

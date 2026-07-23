# Anatomy of a boot

An annotated walk through one real boot of Apple's iPhone OS 3.1.3 kernel on
iOS3-VM: from the reset vector to the last driver that starts, with the emulated
device behind each stage.

> **The stage-by-stage narrative is historical; the frontier note is current.**
> The original run below ended at a missing VFP `VLDMIA`. Current real firmware
> has crossed that wall and the later `0xe1630381` ARMv5TE multiply stop. A resume
> chain then reached the 2.9 B configured cap and wrote checkpoints at 2.4, 2.7
> and 2.85 B, with no panic or emulator undefined stop. A diagnostic continuation
> reached 2,944,340,624 instructions and stopped on ARMv6 `UXTB16`
> (`0xe6cf3073`) in user mode. The complete paired-extend implementation then
> replayed through that stop, wrote a 2.97 B checkpoint and reached a clean
> 2.98 B cap. Free pages dipped to 97 and ended at 214 against a target of 250.
> The current memory-safe external-md evidence is run07: a fresh 128 MiB cold
> boot reached a clean 2 B cap after launchd, fsck, the root mount,
> `mDNSResponder` Seatbelt setup, and `systemShutdown false`, with 12,983 pages
> (50.71 MiB) at its low-water sample. Its framebuffer was disabled and CLCD
> status/mask/scanning were zero. Neither path proves a SpringBoard frame. The
> installable app runs a synthetic guest through CoreGraphics and has no
> real-boot session, touch, audio or guest networking.

Everything here is from actual historical runs. The command below is the recipe,
not a promise of byte-identical current output: the stopping point and log are
tied to that run's source/configuration, and current output may differ. Supply
your own IPSW-derived inputs (see [BOOT_CHAIN.md](BOOT_CHAIN.md)):

```sh
build/core/bootkernel firmware/kernel.macho \
    -d firmware/devicetree.bin \
    -c "debug=0x8 serial=1 nand-enable-adm=0" \
    -r firmware/rootfs.img -R 512 \
    -n 400000000
```

Three parts of that command line are not preferences:

- **`-R 512`.** `arm_vm_init` hardcodes `virtual_avail = 0xe0000000`, so the
  kernel's physical-linear window is exactly 512 MiB. Advertising more makes
  `zone_virtual_addr` stop early-outing and index a `pv_head_table` that is
  still all zeros during `zone_bootstrap` — a null-zone dereference at ~34 M
  instructions that looks nothing like a RAM-size problem. The physical window
  is exactly `[0x08000000, 0x28000000)` and NOR begins at `0x28000000`; current
  machine initialization rejects the historical 768 MiB configurations before
  execution.
- **`nand-enable-adm=0`.** `AppleS5L8900XADMFMC::start` polls a NAND/DMA ready bit
  that never sets here and panics. The driver's own `probe()` honours this
  boot-arg, so it simply never matches. We boot from a RAM disk; we do not need
  NAND.
- **`-r firmware/rootfs.img`.** The real, decrypted 413 MiB
  (433,274,880-byte) HFSX root filesystem
  from the IPSW, published through `/chosen/memory-map` as `RAMDisk` with `rd=md0`
  appended to the command line.

The loader does not retain a second image-sized host buffer. It opens and sizes
the source, proves the kernel/device-tree/boot-args/RAM-disk/framebuffer ranges
are contained and pairwise disjoint, allocates guest DRAM once, and streams the
rootfs directly into its final range through the retained handle. Source
metadata is checked around the transfer. The roughly 445 MiB grown RAM disk
therefore lives inside the 512 MiB guest allocation instead of beside it.

Three more workarounds are applied automatically and echoed in the run header —
the IORTC 30-second wait patched to zero, the `mbx` node's `compatible` string
broken so the PowerVR driver does not match, and the `sha1` nub un-matched so
`IOCryptoAcceleratorFamily` never installs its hardware-SHA-1 hook (`-S` keeps it,
`-g` keeps MBX, `-K` disables the kernel patch). They are printed rather than
silent on purpose: every one of them is a lie we are telling the guest, and a lie
you cannot see is a lie you will spend a day rediscovering.

Instruction indices are counts of retired guest instructions since reset. They
are deterministic for a given build and inputs, and they move as the emulator
changes — treat them as a map, not a fingerprint. Sections marked *(historical)*
were measured on an earlier, shorter, root-less boot and are kept because the
narrative depends on them.

---

## Stage 0 — what stands in for iBoot

We do not run iBoot to hand off to the kernel. `bootkernel` recreates a subset of
the handoff inputs: loaded segments, boot arguments, device-tree properties and
RAM-disk metadata. This deliberate simplification gets to XNU quickly, but it is
not equivalent to executing iBoot, and the guest can observe the synthesized and
patched inputs described above.

The kernelcache is a real, decrypted, LZSS-expanded ARMv6 Mach-O. Its segments
are placed at their own preferred addresses, physical = virtual − 0xb8000000:

```
  __TEXT           vm 0xc0008000 -> pa 0x08008000   2,117,632 bytes
  __DATA           vm 0xc020d000 -> pa 0x0820d000      98,304
  __HIB            vm 0xc0000000 -> pa 0x08000000      20,480
  __KLD            vm 0xc0260000 -> pa 0x08260000       4,096
  __PRELINK_TEXT   vm 0xc02cd000 -> pa 0x082cd000   5,013,504   ← every kext
  __PRELINK_INFO   vm 0xc0795000 -> pa 0x08795000     245,760
  __LINKEDIT       vm 0xc0261000 -> pa 0x08261000     439,716
  entry            vm 0xc0069040 -> pa 0x08069040
```

`__PRELINK_TEXT` is the whole point of a *kernelcache*: every kernel extension
is already linked into the image. That is why Apple's drivers can start in a run
with no filesystem — they are already in memory before the first instruction.

Then we build the two structures iBoot owns.

**The device tree**, Apple's own format, taken from the same IPSW and patched in
place. iBoot fills in the frequencies at runtime because they depend on the
board; a tree with zeros in them makes the kernel divide by zero or wait
forever, so we write the S5L8900 values:

```
  /device-tree    clock-frequency       -> 103,000,000
  /cpus/cpu0      timebase-frequency    ->   6,000,000
  /cpus/cpu0      clock-frequency       -> 412,000,000
  /cpus/cpu0      bus-frequency         -> 103,000,000
  /cpus/cpu0      peripheral-frequency  ->  51,500,000
  /cpus/cpu0      fixed-frequency       ->  24,000,000
  /memory         reg                   -> {0x08000000, 0x08000000}
```

Every patch is same-length and in place, because this format has no relocation
table: each offset is implicit in the byte stream, so changing a length would
mean rewriting everything after it.

**`boot_args`**, 0x138 bytes at physical 0x087db000, with the kernel entered at
`r0 = &boot_args`. Two fields in it are load-bearing and were established by
experiment rather than assumption:

- `Version` (offset 2) must be **6**. `pe_identify_machine()` does
  `ldrh r3,[r0,#2]; cmp r3,#6` and panics `pe_identify_machine: Epoch Mismatch`
  on anything else.
- `topOfKernelData` (offset 0x10) must be **physical**, not virtual. The kernel
  uses it directly as the base for the page tables it is about to build; passing
  the virtual form put TTBR0 at 0xc07dc018 and the first MMU walk read unmapped
  memory.

---

## Stage 1 — reset to virtual memory

| instr | what |
|---|---|
| 0 | `__start`, MMU off, running from physical 0x08069040 |
| 44,312 | `_PE_init_platform` |
| 44,777 | `_DTInit` — the kernel takes the device tree we handed it |
| 44,790 | `_pe_identify_machine` — the epoch check above |
| 44,798 | `_pe_arm_get_soc_base_phys` — asks the tree where the SoC lives |
| 49,023 | first `_DTGetProperty` (858 calls before the run ends) |
| 56,743 | `_PE_parse_boot_argn` — reads the command line, 67 calls |
| 58,575 | **`_arm_vm_init`** — builds page tables and enables the MMU |

From here the kernel is executing out of its own translation regime at
0xc0000000, in tables it wrote itself, walked by our MMU out of guest RAM through
the ordinary bus — exactly as hardware would.

**And not out of TTBR0 alone.** Eleven instructions into `__start` the kernel
writes `TTBCR.N = 2`, which on ARMv6 splits translation: virtual addresses below
2^(32−N) come from TTBR0, everything above from TTBR1. Kernel text
(0xc0008000–0xc020d000) and the 0xffff0000 vector page therefore live in **TTBR1**,
and `set_mmu_ttb` thereafter rewrites TTBR0 *alone* as it switches user pmaps.
Walking TTBR0 unconditionally survives this whole stage — both registers start at
the same base — and dies at the first `pmap_switch` to a user pmap, hundreds of
millions of instructions later, as an unexplained prefetch-abort storm at
0xffff000c. That was the single longest-lived bug in this project.

This stage is unforgiving in a specific way: everything before `_arm_vm_init` is
physical, everything after is virtual, and a wrong address in `boot_args` does
not fail here. It fails later, somewhere that looks unrelated.

---

## Stage 2 — finding the serial port

The kernel does not know where the UART is. It asks the device tree, and then it
maps it.

| instr | what |
|---|---|
| 127,579 | `_PE_init_kprintf` |
| 129,675 | `_serial_init` |
| 131,527 | asks `pe_arm_get_soc_base_phys` for the SoC base |
| 157,003 | finds the `uart0` node in the tree |
| 157,472 | `_ml_io_map` — maps the register page into kernel space |
| 158,663 | `serial_init` returns 1: **a working console** |
| 160,727 | `_switch_to_serial_console` |
| 163,340 | `_PE_initialize_console` |
| 163,351 | `_initialize_screen` |

`/device-tree/arm-io/uart0` has `reg = {0x4c00000, 0x1000}`, an offset from the
SoC base of 0x38000000 — physical **0x3cc00000**, which is
`core/src/soc/uart.c`. The kernel then programs it exactly as it would program
silicon:

```
  WRITE off 0x00 (ULCON)  = 0x00000003   8 data bits
  WRITE off 0x04 (UCON)   = 0x00000405
  WRITE off 0x28 (UBRDIV) = 0x00010019   the divisor for its baud rate
  READ  off 0x10 (UTRSTAT)= 0x00000006   "transmitter ready"
  WRITE off 0x20 (UTXH)   = 0x00000069   'i'
```

That poll of UTRSTAT before every byte is why our UART reports the transmitter
permanently ready: a device that never says "ready" is a device the guest spins
on forever, and it will not tell you why.

---

## Stage 3 — the first byte

| instr | what |
|---|---|
| 7,895,270 | `_printf` entered, from `_PE_init_iokit+0x1a` |
| 7,895,357 | first `_uart_putc` — the first byte this project ever emitted |

The format string, recovered from `r0`, is `"iBoot version: %s\n"`. The argument
is empty because we did not run iBoot to leave a version string behind, so the
line comes out as `iBoot version: ` — 15 characters that took a real timer to
earn. Before the timer block was right, `mach_absolute_time()` read zero forever
and the kernel never got here at all.

Note the nearly eight million instructions between the console being *ready*
(163,340) and the console being *used* (7,895,270). That gap is the VM system and the
zone allocator coming up: `_zcram`, `_zone_page_alloc`, `_kernel_memory_allocate`,
`_vm_page_grab`. The kernel does an enormous amount of work before it says
anything at all, which is exactly why "no output" is such a poor diagnostic and
why this project instruments call paths instead.

---

## Stage 4 — the heartbeat

| instr | what |
|---|---|
| 241,995 | **FIQ #0** — the first interrupt ever taken by this machine |
| 245,911 | `_machine_startup` |
| 249,822 | `_kernel_bootstrap` |

The interrupt path is worth spelling out because three separate components have
to agree, and each was wrong at some point:

- `_pe_arm_init_interrupts` programs **timer 4** at offsets 0xA0–0xAF in the
  block at physical 0x3e200000 (`/device-tree/arm-io/timer`,
  `core/src/soc/timer.c`), and routes **VIC line 7 to FIQ** — not IRQ.
- `_s5l8900x_set_decrementer` writes the next deadline to 0xA8.
- `_fleh_fiq_s5l8900x` acknowledges by writing `0x00030000` to the latch. Our
  acknowledge mask has to be exactly that: latch any bit the handler's write
  does not clear and the line stays asserted, the handler re-enters immediately,
  and the boot hangs with no diagnostic at all.

Separately, `_s5l8900x_get_timebase` reads 0x080/0x084 as a **free-running
64-bit counter** — this is `mach_absolute_time()`. It must count whether or not
any timer is armed; gating it on timer 4's enable bit reads zero through all of
early boot.

The steady-state cadence, from the log:

```
  FIQ #1  @instr 13,991,177
  FIQ #2  @instr 18,106,370   gap 4,115,193   t4_count 59,930
  FIQ #3  @instr 22,221,564   gap 4,115,194   t4_count 59,930
```

59,930 ticks at 6 MHz is 9.99 ms; 4,115,193 instructions at 412 MHz is 9.99 ms.
The kernel's 100 Hz scheduler tick and the emulator's instruction budget agree,
which is the whole content of the timebase-ratio fix: feed the timer at the
guest's real cpu:timebase ratio (one tick per ~68 instructions) instead of 1:1.
Before that fix this same boot took **1,939,179** FIQs and spent 65.9% of its
instructions inside the handler, because the kernel could never service a
deadline before the next one had already passed. The 400 M-instruction boot now
takes **385** FIQs — 38,235 instructions, 0.0% of the run, longest single entry
344 instructions.

---

## Stage 5 — IOKit, and Apple's drivers meeting our hardware

This is the part that makes the whole exercise real. IOKit walks the device tree
we supplied, matches the prelinked kexts against the nodes it finds, and starts
them. They then program registers — and what is on the other side of those
registers is C in this repository.

The console output, in order, with what each driver is actually talking to:

| Apple's kext says | device-tree node | physical | on our side |
|---|---|---|---|
| `AppleS5L8900XIO::start: chip-revision: EVT0` | `/arm-io` | 0x38000000 | the SoC nub itself |
| `AppleARMPL192VIC::start: _vicBaseAddress = 0xe38ed000` | `/arm-io/vic` | 0x38e00000 | `soc/vic.c` — 2 reads, 21 writes |
| `AppleS5L8900XEdgeIC::start: 0xe38e6000` | `/arm-io/edgeic` | 0x38e02000 | **not modelled** — 2 writes, counted |
| `AppleS5L8900XGPIOIC::start: 0xe38f5000` | GPIO IC | 0x39a00080 | shares the power page; GPIO proper at 0x3e400000, 27 writes, not modelled |
| `AppleS5L8900XPowerController::start: 0xe38fd000` | `/arm-io/power` | 0x39a00000 | `soc/power.c` — 7 reads, 21 writes |
| `AppleS5L8900XClockController: Dynamic Performance State Management Enabled with max state 3` | `/arm-io/clkrstgen` | 0x3c500000 | **not modelled** — 1 read, 6 writes |
| `AppleARMPL080DMAC::start: dmac0 / dmac1` | `/arm-io/dmac0`, `dmac1` | 0x38200000, 0x39900000 | mapped, no register traffic yet |
| `AppleS5L8900XADM::start: mapped I/O registers at 0xe9915000/0x38800000` | `/arm-io/adm` | 0x38800000 | mapped, no register traffic yet |
| `AppleS5L8900XSDIO::start(): SDIO Revision 8900X` … `registers @ paddr 0x38d00000` | `/arm-io/sdio` | 0x38d00000 | **not modelled** — 1 read, 6 writes |
| `AppleS5L8900XSPIController::start: spi0 / spi1` | `/arm-io/spi0`, `spi1` | 0x3c300000, 0x3ce00000 | mapped, no register traffic yet |
| `AppleS5L8900XUSBPhy::start registers at 0xea942000` | `/arm-io/otgphyctrl` | 0x3c400000 | mapped, no register traffic yet |
| `AppleS5L8900XI2CController::start: i2c0 / i2c1` | `/arm-io/i2c0`, `i2c1` | 0x3c600000, 0x3c900000 | i2c0 **not modelled** — 7 reads, 7 writes |
| `AppleS5L8900XTimer::start: 0xea94a000` | `/arm-io/timer` | 0x3e200000 | `soc/timer.c` — 6,447 reads, 254 writes |
| `AppleS5L8900XWatchDogTimer` + `AppleARMWatchDogTimer installing handlePEHaltRestart handler` | `/arm-io/wdt` | 0x3e300000 | mapped, no register traffic yet |
| `AppleS5L8900XI2SController::start: i2s0 / i2s1` | `/arm-io/i2s0`, `i2s1` | 0x3ca00000, 0x3cd00000 | mapped, no register traffic yet |
| `AppleMPVDDriver::init / ::start` | `/arm-io/mpvd` | 0x39600000 | video decoder; mapped only |
| `AppleMBXDevice(0xc0bf4800): Init` | `/arm-io/mbx` | 0x3b000000 | the 2D/3D block; mapped only |
| `ApplePCF50635PMU::start: pmu _pmuIICNub` | on i2c0 | via 0x3c600000 | the PMU speaks I²C; consistent with the i2c0 traffic above |
| `AppleMicron2020::start()` / `Registering IOCameraSensor service.` | camera | — | the sensor, over I²C |
| `AppleBaseband: Could not find mux function` | `/arm-io/spi2` | 0x3d200000 | **deliberate** — we emulate the application processor, not the modem |

**That table is the 200 M-instruction boot *(historical)*, and it is now the
short version.** Honouring `TTBCR.N`/`TTBR1` in the MMU started the rest of the
tree — the boot had been stopping at the first `pmap_switch` to a user pmap,
which deleted kernel text and the vector page from a walk that only ever
consulted TTBR0, and stormed on prefetch aborts at 0xffff000c forever. What the
same UART carries now, in order and unedited apart from elision:

```
BSD root: md0, major 2, minor 0
AppleBaseband::start(0xc07c4a00): baseband
AppleS5L8900XIO::start: chip-revision: EVT0
AppleARMPL192VIC::start / AppleS5L8900XEdgeIC::start / AppleS5L8900XGPIOIC::start
AppleS5L8900XPowerController::start / AppleS5L8900XClockController::start
AppleARMPL080DMAC::start: dmac0 / dmac1
AppleS5L8900XADM::start: mapped I/O registers at 0xe316a000/0x38800000
AppleS5L8900XWatchDogTimer::start / AppleARMWatchDogTimer installing handler
IOSDIOController::init(): IOSDIOFamily-24.7 Dec 18 2009 01:49:48
AppleS5L8900XSDIO::start(): SDIO Revision 8900X
AppleS5L8900XI2CController::start: i2c0 / i2c1
AppleMPVDDriver::init / ::start
AppleS5L8900XI2SController::start: i2s0 / i2s1
AppleS5L8900XSPIController::start: spi0 / spi1
AppleS5L8900XUSBPhy::start registers at 0xe9fca000
AppleS5L8900XTimer::start: _timerBaseAddress: 0xe9fd2000
com.apple.AppleFSCompressionTypeZlib load succeeded
virtual bool AppleMobileFileIntegrity::start(IOService*): built Dec 21 2009 08:27:49
L2TP domain init / PPTP domain init
Jettisoning kext bootstrap segment.
AppleMicron2020::start() / Registering IOCameraSensor service.
ApplePCF50635PMU::start: pmu _pmuIICNub: 0xda970e80
AppleARMPL080DMAC::_initDMAChannel: index: 0..7  (twelve channel inits)
AppleS5L8900XSerial: Identified Serial Port on ARM Device=uart0/uart1/uart3/uart4
AppleSerialMultiplexer: mux::start: created new mux (18) for spi-baseband
AppleMultitouchZ2SPI: successfully started
AppleMultitouchZ2SPI: using DMA for bootloading
IOSDIOController::enumerateSlot(): Searching for SDIO device in slot: 0
[0.567924666]: AppleS5L8900XSDIO::sendCommand(): Timeout waiting for CMDRDY
IOSDIOController::enumerateSlot(): CMD5 failed with SDIO device on slot 0
AppleS5L8900XSDIO::enumerateCards(): Unable to enumerate SDIO device
```

4,595 bytes from unmodified Apple kernel and kext code. Three lines are worth
naming: `AppleMobileFileIntegrity` — the code-signing enforcer — starts cleanly;
`AppleMultitouchZ2SPI` reaches its bootloading/DMA request, which proves execution
of that driver path but not a usable touch device; and SDIO reaches the mapped
command/poll path before timing out. The timeout does not establish working card,
interrupt, timing or error semantics.

Two things in the historical table are worth dwelling on, and both still hold.

**"Mapped, no register traffic yet" is the honest state, not a summary.** Those
drivers matched, ran their `start()`, called `ml_io_map` to get a virtual window
onto their registers, printed the address — and then did not touch it in the
remainder of this run. That is a real observation about how far each driver
gets, and it comes from counting every non-RAM access rather than from reading
the log's prose.

**"Not modelled" is visible, not silent.** In this run 19 reads and 772 writes
went to 10 pages nothing answers for. They are counted, attributed to a PC, and
reported by page. That is the entire reason the power controller was findable:
one page absorbed 3,887,707 reads because a driver was polling a status bit that
would never change, and it showed up as a line in a report instead of as a boot
that mysteriously took forever.

The complete list of physical pages the kernel touched outside RAM — **22** of
them now, in the 400-million-instruction boot with the root filesystem. The
right-hand column is the *first PC* that reached each page, and this is the
report that changed most in the last session: it used to bottom out at a bare
address, and now it names the kext.

```
  0x3cc00000  r=4595   w=4601   uart0      _PE_init_kprintf+0x9c
  0x3e200000  r=87963  w=1304   timer      _pe_arm_init_interrupts+0xba
  0x38e00000  r=88     w=292    vic0       _pe_arm_init_interrupts+0xf8
  0x38d00000  r=10003  w=10     sdio       AppleS5L8900XSDIO+0x118c
  0x3d000000  r=3      w=702    pke        AppleS5L8900XCrypto+0x3930
  0x39a00000  r=18     w=38     power      AppleS5L8900X+0x24e8
  0x3e400000  r=0      w=49     gpio       AppleS5L8900X+0x24d0
  0x3c500000  r=1      w=19     clkrstgen  AppleS5L8900X+0x5e54
  0x38e01000  r=0      w=16     vic1       AppleARMPL192VIC+0x1290
  0x38e02000  r=0      w=3      edgeic     AppleS5L8900X+0x56a0
  0x3c600000  r=7      w=7      i2c0       AppleS5L8900X+0x3e2c
  0x3c900000  r=7      w=7      i2c1       AppleS5L8900X+0x3e2c
  0x3c300000  r=0      w=13     spi0       AppleS5L8900X+0x4978
  0x3ce00000  r=0      w=19     spi1       AppleS5L8900X+0x4978
  0x3cc04000  r=8      w=15     uart1      AppleS5L8900XSerial+0x20f8
  0x3cc0c000  r=10     w=17     uart3      AppleS5L8900XSerial+0x20f8
  0x3cc10000  r=8      w=15     uart4      AppleS5L8900XSerial+0x20f8
  0x38c00000  r=0      w=42     (crypto)   AppleS5L8900XCrypto+0x14c8
  0x38000000  r=6      w=11     (arm-io)   AppleS5L8900XCrypto+0x2b24
  0x38200000  r=1      w=2      dmac0      AppleARMPL080DMAC+0x1d08
  0x38100000  r=2      w=0      unidentified — no device-tree node at this offset
  0x00000000  r=1      w=0      _PE_create_console reading the framebuffer base
```

`0x38d00000` is the interesting one: 10,003 of its 10,013 accesses are
`IOSDIOController::enumerateSlot` polling for a CMD5 response from a card that
does not exist. It times out and reports so — a device correctly failing is not
the same thing as a device that hangs, and the report distinguishes them.

For addresses inside a kext, the resolver can go as far as `<bundle-id>+0xNNNN`
and no further, and that is a hard limit rather than a to-do: the kernelcache
builder strips each prelinked kext's `LC_SYMTAB`, so there are no per-kext
function names to find. None of the kernel's 11,430 symbols fall inside
`__PRELINK_TEXT`. See [debugging.md](debugging.md).

---

## Stage 6 — BSD, and the root filesystem

| instr | what |
|---|---|
| 64,567,734 | **`_bsd_init`** |
| 81,654,150 | first `_kprintf` / `_serial_putc` |
| 116,573,687 | first data abort: FSR 0x07, DFAR 0xea110000, in `IOBufferMemoryDescriptor::initWithPhysicalMask` |
| ~73.5 M *(measured separately)* | `_IOFindBSDRoot` → `_mdevadd` → **`BSD root: md0, major 2, minor 0`** |

That last line is the one M4 was for. The kernel walked its own storage stack,
found the RAM disk we published in `/chosen/memory-map`, and mounted a real
413 MiB HFSX volume out of Apple's own IPSW as its root.

Getting there needed two things that were not obvious. `bsd_init` calls
`IOKitInitializeTime`, which does `waitForService(resourceMatching("IORTC"),
&{tv_sec = 30})` — and `IORTC` is never published, because the PMU's real-time
clock is not modelled. Thirty seconds of guest time is a very long silence.
Patching that timeout to zero reaches `IOFindBSDRoot`. And the 512 MiB `memSize`
cap above is what stops early VM init from faulting once a 413 MiB disk is
actually present.

Where the instructions actually go, sampled every 1,024 instructions:

```
   17.0%  OSDictionary::getObject(OSSymbol const*)
    5.2%  _mac_file_label_init
    3.5%  _lck_mtx_unlock
    3.3%  OSMetaClass::checkMetaCast
    3.0%  _strncmp
    2.6%  OSObject::taggedRelease
    2.3%  _lck_mtx_lock
    2.3%  OSSymbolPool::findSymbol
```

That profile is a portrait of IOKit matching: string interning, dictionary
lookups and metaclass casts, over and over, as the registry is built and every
prelinked personality is compared against every node. It is what a healthy 2009
kernel bringing up its driver stack looks like, and it is the strongest single
piece of evidence that the emulation is not merely *not crashing* but doing the
right work.

Note the report's own warning in this run: 77,858 samples were dropped because
the profiler's function table filled at 1,024 entries, and it says so rather than
quietly printing a plausible-looking distribution. An earlier version of this
profiler silently dropped everything past 64 entries and printed identical output
at 200 M and 400 M instructions — which looked exactly like coverage.

`_panic` and `_Debugger` are never reached. 48 distinct abort sites remain, all
FSR 0x07 on a marching sequence of kernel virtual addresses in
`IOBufferMemoryDescriptor::initWithPhysicalMask` and the kernel's own
`_fleh_dataabt`; the kernel takes them and continues. "Survivable and
unexplained" is on the list of things to explain rather than a thing to be
pleased about.

---

## Stage 7 — pid 1, and where the historical pre-VFP run stopped

`bsdinit_task` runs, `load_init_program` opens `/sbin/launchd` off the volume we
just mounted, and `execve` gets remarkably far.

| instr | what |
|---|---|
| 230,812,220 | `_bsdinit_task` |
| 230,864,582 | `_load_init_program` |
| 230,895,729 | `_execve` |
| 230,968,564 | `_mac_vnode_check_exec` — the MAC policy is consulted and allows it |
| 231,010,531 | `_grade_binary` (3 calls) — the armv6 Mach-O is graded, not rejected |
| 231,011,045 | `_load_machfile` — launchd is mapped |
| 231,049,078 | `_ubc_cs_blob_add` (2 calls) — its signature blobs are registered |
| 232,201,298 | `_cs_validate_page` (15 calls), `cs_validate:hashing` (15) — and `bad_hash` / `no_hash_exit` **never reached**: every page validates |
| 233,031,366 | **`_fleh_swi`** (24) — the first system call instruction pid 1 ever executes |
| 233,347,392 | `_mach_msg_overwrite_trap` (12) |
| 234,013,919 | **`_unix_syscall`** (5) |
| 234,731,379 | `_fleh_undef` (1) → `_sleh_undef` → `_vfp_trap` |
| **234,731,493** | **the emulator stops: UNDEFINED INSTRUCTION, `0xecb10a20`, lr `_vfp_trap+0x38`** |

**pid 1 executed user-mode code and made system calls.** `_panic` was never
reached in this run. It did not end on a guest panic — the emulator stopped on
an instruction it did not then implement.

XNU does not leave VFP enabled: `_init_vfp` grants CP10/CP11 access once, and
after that the gate is `FPEXC.EN` alone, cleared per thread. So the first VFP
instruction any thread executes is *supposed* to take an Undefined exception,
which `_fleh_undef` → `_sleh_undef` → `_vfp_trap` → `_vfp_switch` turns into
"enable VFP and re-run it". That whole path worked in this run. What stopped
this run was the instruction `_vfp_switch` itself uses: `0xecb10a20` decodes as
`VLDMIA r1!, {s0-s31}`, the load-multiple that restores a thread's VFP register
file. The interpreter correctly returned `ARM_UNDEFINED` rather than guessing.
That VFP family is implemented and covered by regression tests now, so this is
no longer the current blocker.

Keep the scale honest: in this trace, five BSD system calls and twelve Mach
traps was not a userland. Nothing had been logged by userspace and no daemon had
started. Later work added the CLCD/panel path; it did not turn this historical
trace, or the app's synthetic demo, into SpringBoard.

Three walls on this exact path are worth recording, because each looked like a
completely different problem from its symptom:

- The kernel first reached this code and **livelocked on ~2.8 million identical
  data aborts** at `_copyout+0x40`, one every ~395 instructions, because we never
  set `DFSR.WnR`. XNU therefore repaired every write fault as a read fault,
  forever. The very first unprivileged write the kernel performs is the `copyout`
  of the string `"/sbin/launchd"`.
- Then `execve` returned **errno 86, `EBADARCH`**, on a disk where all 385 ARM
  Mach-Os are cputype 12 / cpusubtype 6. The disk was fine; we were returning
  zero for `ID_ISAR1`, so the kernel's Jazelle probe failed, its architecture
  field stayed 0xF, and `cpu_subtype` fell through to `CPU_SUBTYPE_ARM_ALL` —
  which `grade_binary`'s jump table does not cover.
- Then **launchd's first text page failed its signature**, and it spun
  `cs_invalid_page` → `psignal` ~95,000 times. This looked exactly like a corrupt
  disk image, and it was not. `cs_validate_page` hashes exactly 4096 bytes, and
  `SHA1UpdateUsePhysicalAddress` routes exactly-4096-byte buffers to a **hardware**
  SHA-1 engine whenever `_performSHA1WithinKernelOnly` is non-NULL — a hook
  installed by `IOCryptoAcceleratorFamily`, which matched in our boot. The engine
  at 0x38000000 is not modelled, so six reads came back as whatever the stub
  returned and `SHA1Final` emitted that.

  Two private, untracked historical verifications exonerated the image *before*
  anything was changed — a UDIF verifier over all 7 `blkx` tables and every
  per-`blkx` CRC32, and an HFSX reader that reported code-directory page hashes
  for all 155 signed Mach-Os and 6,731 code pages on the volume. They reported
  zero mismatches, launchd 46/46 and dyld 56/56. The verifier tools and outputs
  are not present in the public tree, so this is recorded evidence rather than
  a reproducible current check.

  **The clinching evidence was timing.** `SHA1Transform` costs ~2,262 Thumb
  instructions per 64-byte block, so hashing 4 KB in software must cost ~145,000
  instructions. The observed `SHA1Init` → verdict interval was **14,329** — 10.1x
  too few. Software SHA-1 provably never ran, which located the bug without
  disassembling anything further.

### Stage 8 — the fstab wall, and why launchd was halting the machine

Once launchd ran, the boot ended in a *clean guest reboot*: `_halt_all_cpus`,
with `_panic` never reached. That is not a kernel fault, it is launchd giving
up. The console said:

```
Running fsck on the boot volume...
/dev/disk0s1: No such file or directory
/dev/disk0s1: CAN'T CHECK FILE SYSTEM.
/dev/disk0s1 (hfs) EXITED WITH SIGNAL 8
fsck failed!
```

The guest's `/private/etc/fstab` is 76 bytes and reads:

```
/dev/disk0s1 / hfs ro 0 1
/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2
```

**Neither device can exist on this machine.** `disk0` on an iPhone1,2 is
published by `AppleNANDFTL` (raw NAND → a linear logical space) and cut into
`disk0s1`/`disk0s2` by **`IOFlashPartitionScheme`**, which fails its probe
unless the provider carries a `boot-from-nand` property and then validates a
*magic* and a *major version* on an on-media partition table:

```
IOFlashPartitionScheme::%s: ERROR: magic on partition table, 0x%08X, doesn't match expected value, 0x%08X
IOFlashPartitionScheme::%s: ERROR: major version on partition table, 0x%08X, does not match driver, 0x%08X
```

Both that table and the FTL's own on-media format are undocumented, so per the
project rule we do not synthesise them. We boot the system volume as the RAM
disk `md0` instead, and there is no `disk0` of any kind.

Be precise about *which* piece is undocumented, because it is easy to conclude
too much here. Partitioning in general is fine: `IOStorageFamily` in this
kernelcache carries built-in **`IOGUIDPartitionScheme`** and
**`IOFDiskPartitionScheme`** personalities (provider `IOMedia`,
`IOPropertyMatch { Whole = true }`, and an FDisk content table that maps type
`0xAF` to `Apple_HFS`), so any block device that does get published can be cut
up with an ordinary GPT or MBR. What is missing is anything that would publish
that `IOMedia` **during the launchd bootstrap** — there is no
`IOUSBMassStorageClass` and no SCSI stack in this kernelcache, and the
kernel-side DiskImages entry point `di_root_image()` is called solely by
`imageboot`/`netboot`, neither of which is compiled in. The DiskImages stack
itself is alive and does have userland clients on the system volume —
`/usr/libexec/mobile_image_mounter` and `/usr/libexec/debug_image_mount` both
name `IOHDIXController` and `hdik-unique-identifier` — but they are lockdownd
services that attach a `.dimage` on host request, onto `/Developer`, long
after fstab has been read. The kernel also has exactly one memory device:
`IOFindBSDRoot` reads a single `RAMDisk` property under a static `didRam`
guard, so there is no `md1` to be had either.

Disassembling `launchctl`'s `_bootstrap_cmd` (launchd-321) settles exactly what
launchd wants, and the distinction turns out to be the whole answer — **fsck is
fatal, mount is not**:

```c
statfs("/", &sfs);
if (sfs.f_flags & MNT_RDONLY) {          /* xnu mounts EVERY root MNT_RDONLY */
    if (!is_safeboot()) fputs("Running fsck on the boot volume...\n", stdout);
    if (fwexec(fsck -p) == -1 && fwexec(fsck -fy) == -1) {
        fputs("fsck failed!\n", stdout);
        reboot(RB_HALT);                 /* <-- the halt. unconditional. */
    }
    path_check("/etc/fstab") ? fwexec(mount -vat nonfs)
                             : fwexec(mount -uw /);
    /* ... every failure from here on is only _log_launchctl_bug() ... */
}
```

Three facts fall out of that, each checked against the binaries on our own
rootfs rather than assumed:

- `fwexec()` returns −1 unless the child exits with status 0, so `/sbin/fsck`
  really must succeed. `/sbin/fsck` is the BSD wrapper: it reads `/etc/fstab`
  and checks every entry with a nonzero pass number, so it inherits whatever
  the file says. Deleting the file does not help — it has
  `"Can't open checklist file: %s"` and exits non-zero.
- The `MNT_RDONLY` test is on `struct statfs.f_flags` at **offset 0x40**, which
  identifies it as the 64-bit-inode `struct statfs`. If `/` is already
  read-write, launchd skips this entire block.
- `mount(8)`'s `ismounted()` compares **both** `f_mntfromname` and
  `f_mntonname`, and xnu's root mount has `f_mntfromname == "root_device"` (set
  in `vfs_rootmountalloc_internal`, which is also where the unconditional
  `MNT_RDONLY` comes from). So an fstab entry for `/` is *not* skipped as
  already-mounted, and with `update` in its options it becomes a genuine
  `MNT_UPDATE` remount.

So the VM rewrites the record in the **loaded copy** of the RAM disk to name
the device it actually provides (`tools/bootkernel.c`, `rd_rewrite_fstab`; the
image on disk is never touched, and the patch refuses unless the stock 76 bytes
appear exactly once):

```
/dev/md0 / hfs rw,update 0 1
```

`pass 1` keeps Apple's own `fsck_hfs` in the loop rather than skipping the
check the hardware would have done — and it costs nothing, because the volume
carries `kHFSVolumeUnmounted` and `fsck_hfs -p` quick-exits on a clean volume.
The result:

```
Running fsck on the boot volume...
/dev/md0 on / (hfs, local, noatime)
launchctl: Couldn't stat("/etc/mach_init.d"): No such file or directory
```

`mount -v` prints `read-only` when it means it; its absence is the proof that
`/` is now mounted **read-write** — which on hardware is what `disk0s2` would
have supplied for `/private/var`. launchd clears the whole crucial-path table
(`/tmp`, `/var/tmp`, `/var/folders`, `/var/db/launchd.db` all already exist on
the volume) and goes on to load LaunchDaemons.

Two residual complaints on that path are cosmetic and worth naming so nobody
chases them:

- `Bug: launchctl.c:3793 ... sysctl(nbmib, 2, ...) == 0` — `nbmib` is
  `{CTL_KERN, 40}` = `KERN_NETBOOT`, a node this kernelcache does not register
  because its NFS client is compiled out. The failure path branches to the
  *same* instruction as the `netbooting == 0` success path, so it changes
  nothing.
- `Bug: launchctl.c:3094 ... value != NULL` — the `boot-args` property lookup
  on `IODeviceTree:/options`, used only to decide whether the boot is verbose.

### The volume had zero free blocks

`freeBlocks == 0` in the volume header and every one of the 105,780 bits in the
allocation bitmap set: Apple ships the system dmg sized exactly to its contents,
because on hardware `/` is `disk0s1` and stays read-only forever while
everything writable lives on `disk0s2` — a volume the restore `newfs`es and
which this machine does not have. So `/` was writable and nothing could be
*allocated* on it.

`bootkernel --grow <MB>` (default **32**, `0` disables) grows the volume in the
**loaded copy** of the RAM disk, so `firmware/rootfs.img` stays as it came out
of the IPSW. HFS+ is specified in Apple's TN1150 and the image is a bare,
unencrypted volume, so this is the documented layout applied rather than a
guess at one — four edits, in `rd_grow_volume()`:

1. `volumeHeader.totalBlocks += n`;
2. `volumeHeader.freeBlocks` **recounted from the bitmap**, never derived from
   the growth, so the header cannot end up describing a bitmap that was not
   written;
3. the allocation block holding the **reserved tail** moves: the old one is
   freed, the new one allocated;
4. the alternate volume header is rewritten at `totalBlocks × blockSize − 1024`.

Step 3 is the one with a trap in it. TN1150 puts the alternate volume header
1024 bytes before the end of the volume and reserves the final 512; the block
containing that tail is marked in use and belongs to no file. Walking the
catalog and the extents overflow file over this image accounts for 105,778 of
the 105,780 in-use blocks, and the two left over are exactly block 0 (boot
blocks + primary header) and block 105,779 (the alternate) — and no file
*could* own that last block, because extents are whole allocation blocks, so an
owner would own the alternate header's bytes and be scribbled on at every
flush. When the tail moves, that block becomes ordinary free space.

The 16 KiB allocation file is the ceiling: 16384 × 8 = 131,072 bits, a 512 MiB
volume at this block size. `--grow` clamps there and says so. (The headroom
exists because `newfs_hfs` rounded 105,780 bits up to a whole 4-block clump;
TN1150 allows a bitmap larger than the volume needs and requires only that the
surplus bits be zero, which they are.)

Rebuilding the bitmap from scratch — a full catalog walk, which is what
`fsck_hfs` phase 5 does — reproduces the on-disk bitmap exactly on all 113,971
blocks of the grown volume, and `freeBlocks` matches the recount: 8191 blocks,
32.00 MiB.

**What the guest says about it.** `fsck_hfs -p` accepts the grown volume and
launchd goes on to remount `/` read-write, which is the kernel's own HFS mount
code independently accepting the new `totalBlocks` against the new device size
(`md0`'s size comes from the same grown buffer, so the volume exactly fills the
device — which is where `fsck_hfs` looks for the alternate header). But
`fsck_hfs -p` **quick-exits on a volume carrying `kHFSVolumeUnmounted`**, so
that is a weaker check than it sounds.

In the historical run, forcing the full one — clearing the clean bit so preen
had to do the real scan — exposed a CPU gap rather than a volume defect:

```
Running fsck on the boot volume...
=== ABNORMAL STOP: UNDEFINED INSTRUCTION ===
  pc 0x00013130  cpsr 0x60000010 (User, ARM)
```

`0x13130` is `fsck_hfs+0x12130` (its `__TEXT` is at `0x1000`), and the word
there is `0xe1660385` — **`SMULBB r6, r3, r5`**. At that commit the interpreter
trapped the DSP-multiply space deliberately. Current source implements and tests
the complete related ARMv5TE set (`SMULxy`, `SMLAxy`, `SMLALxy`, `SMULWy`, and
`SMLAWy`), so this exact instruction gap is closed. The forced-dirty full-check
scenario has not yet been rerun end to end, so the honest current claim is
"opcode implemented", not "full fsck completed".

So the volume is checked where it actually lives instead. Snapshotting the
machine at 2.9 G instructions and reading the volume header and allocation
bitmap straight out of guest DRAM, at the RAM disk's own physical address:

```
LIVE GUEST VOLUME HEADER (guest DRAM, 2.9 G instructions in)
  signature HX  version 5  blockSize 4096
  totalBlocks 113971   freeBlocks 8191   nextAllocation 105779
  attributes  0x00000000
live bitmap: 105780 of 113971 blocks in use, 8191 free
bits set in the newly-added range 105779..113970: [113970]
```

Every number is the one written into the image, unchanged after billions of
instructions of guest execution — and the one bit set in the new range is the
new alternate volume header's block, exactly where TN1150 puts it. The
interesting field is `attributes`, which the image carries as `0x00000100`
(`kHFSVolumeUnmounted`) and the guest has **cleared**: that is the kernel's own
`hfs_mountfs` marking the volume mounted and flushing the header back. Writes
to `md0` reach the volume, and the volume they reach is the grown one.

`freeBlocks` is still the full 8191, so nothing had been allocated yet at that
point — though HFS+ updates the on-disk header lazily, so that is evidence of
"no sync since a write" as much as "no write".

**The free space did not, by itself, change the historical comparison.** Run
out to 3 billion instructions with and without `--grow`, the console was
identical line for line.
`_execve` stays at 11 either way — but that number was never measuring what it
looked like it was measuring:

```
mDNSResponder[14] syscall_builtin_profile: mDNSResponder (seatbelt)
mDNSResponder[14] Builtin profile: mDNSResponder (seatbelt)
```

**A LaunchDaemon is running, as pid 14, in both runs.** launchd starts its jobs
with `posix_spawn`, not `execve`, so the `_execve` probe never sees one; the 11
hits are `load_init_program` plus the `fwexec()` helpers of launchd's own
bootstrap. Nothing was stuck at 11. Both runs simply need ~3 G instructions
rather than 800 M to get there, and both then stall in the same place.

### Where the free space has to come from

Growing the volume comes straight out of the guest's free page pool, because
the RAM disk is static memory below `topOfKernelData`: 90.93 MiB before,
58.93 MiB after, at the documented `-R 512`.

`topOfKernelData` is a single **line**, not a list — everything below it is
static — so a RAM disk placed *below* the kernel image is exactly as protected
as one placed above it, and stops pushing that line up by the size of the root
filesystem. `-Y` does that, and needs `-V` to open a gap under the kernel
(`phys = vmaddr − virt_base + phys_base`). Measured:

| flags | RAM disk | free page pool | volume free | reaches |
|---|---|---|---|---|
| `-R 512` (documented boot command) | 445 MiB | 58.93 MiB | 32 MiB | launchd, daemons |
| `-V 0xa4000000 -R 768 -Y` (historical; now rejected) | 445 MiB | 312.14 MiB | 32 MiB | `BSD root: md0`, then idle |
| `-V 0xa0000000 -R 768 -Y --grow 100` (historical; now rejected) | 512 MiB | 248.14 MiB | 98 MiB | `BSD root: md0`, then idle |

The two 768 MiB rows are retained as historical observations from older source,
not valid current configurations. The current machine constructor rejects a RAM
aperture that overlaps a decoded device. NOR begins at `0x28000000`, so SDRAM
starting at `0x08000000` has a maximum non-overlapping size of 512 MiB. The
allocation file also caps this volume layout at 512 MiB.

**The last column was the historical point.** In those older runs both `-V` rows
reached `BSD root: md0` and then went idle without reaching
`_load_init_program`, while the documented 512 MiB run reached it at about 225 M
instructions and execed launchd. A `gVirtBase` below the kernel's compiled-in
`VM_MIN_KERNEL_ADDRESS` (`0xc0000000`) was already unusable; current source also
rejects the overlapping 768 MiB physical map before boot. The old 59-versus-312
MiB comparison illustrates the memory pressure that motivated `-Y`, not an
available current configuration.

---

## Stage 9 — current snapshot-resume frontier

Two interpreter changes make the current instruction counts different from the
older narrative without making them less meaningful.

First, XNU's exact ARM1176 wait-for-interrupt instruction no longer retires in a
host loop while nothing happens. The CP15 WFI callback advances the timer and
CLCD only to the earliest enabled VIC edge that can wake the processor, while
the CPU's retired-instruction count remains unchanged. If no future event is
known it falls back safely. Snapshot triggers therefore remain absolute retired
instruction positions rather than a mixture of work and fabricated idle spins.

Second, the next exact user-mode stop after VFP was decoded rather than patched
as a one-off:

```text
pc       0x33dba604
encoding 0xe1630381
decode   SMULBB r3, r1, r3
```

That belongs to the ARMv5TE signed DSP multiply group. Current source implements
the full related family — `SMULxy`, `SMLAxy`, `SMLALxy`, `SMULWy`, and `SMLAWy`
— including top/bottom half selection, signed word-by-halfword truncation,
sticky-Q overflow behavior, legal aliases and fail-closed invalid forms. This
cleared `0xe1630381`; it was not replaced with a hard-coded result.

**Measured current continuation:** the first three intervals below ended at
their configured cap. The fourth stopped fail-closed on the named user-mode
instruction rather than guessing its semantics; the fifth replayed through the
fix to its cap. `_panic` and `Debugger` were not reached in any interval.

| restore → cap | checkpoint written | new evidence | free-page report |
|---|---|---|---|
| 2.2 B → 2.45 B | 2.4 B | crossed `0xe1630381`; `launchd` and `mDNSResponder` alive | 2,004 pages / 7.83 MiB; low 1,999 |
| 2.4 B → 2.8 B | 2.7 B | one new `_execve`, first at 2,605,595,575; `systemShutdown false` | 542 pages / 2.12 MiB; low 539 |
| 2.7 B → 2.9 B | 2.85 B | no panic or undefined stop | 317 pages / 1.24 MiB; target 250; low 301 at 2,886,008,832 |
| 2.85 B → 2,944,340,624 | none | stopped on `0xe6cf3073`, ARMv6 `UXTB16 r3, r3`, in user mode | 253 pages / 0.99 MiB; target 250; low 97 at 2,934,505,472 |
| 2.85 B → 2.98 B, paired-extend fix | 2.97 B | cleared `0xe6cf3073`; status `OK`; 2 `_load_machfile`, 400 code validations, 4,266 SWIs, 3,373 Unix syscalls | 214 pages / 0.84 MiB; target 250; low 97 at 2,934,505,472 |

The 58.93 MiB figure above is the earlier post-layout pool, not the amount left
after this much userspace activity. Direct streaming removed a second host-side
copy but the roughly 445 MiB RAM disk remains pinned guest memory below
`topOfKernelData`. The latest run went 153 pages below the guest's free target,
recovered to three pages above it before the former opcode stop, then ended 36
pages below target at 2.98 B. That movement suggests XNU's reclamation path is
active, but the available headroom is still unsafe for the app. The storage
audit also proved that setting md physical mode and adding an external bus
aperture is insufficient: this kernel's `_bcopy_phys` only applies the normal
DRAM direct-map delta. The narrowly scoped writable md-strategy bridge, its
locked file adapter, an exact full-image/ARMv6/LC_UUID/site-gated 7E18 patch
manifest, and a bounded immutable-source HFS work-image provisioner now exist
under unit tests. `bootkernel --external-md` now installs the cold-boot chain:
it exact-gates the original kernel, device tree, and rootfs; creates a no-replace
writable work image; publishes md0 through a synthetic address outside the
128 MiB DRAM aperture; and installs only the two audited strategy-copy exits.
### 2026-07-22: first 128 MiB external-md real-firmware run

Commit `d9d9e40` was run cold to a 400,000,000 retired-instruction cap with the
documented exact 7E18 inputs and default 32 MiB growth. All three identity gates
passed, and the create-only work image was 466,825,216 bytes with SHA-256
`4fb9b51eaca0f52fdba8d2a7909b57eab7e8d5c6e67112f277f501a8af76cc61`.
The immutable source hashes were identical before and after the run.

The guest reported `BSD root: md0, major 2, minor 0`, first entered
`_load_init_program` at 235,856,815, first entered `_execve` at 235,888,017,
and printed `*** launchd[1] has started up. ***` followed by
`Running fsck on the boot volume...`. At the cap the machine reported status
`OK`: no `_panic`, `_Debugger`, raw-mdevrw guard, undefined emulator stop, or
bridge failure. The bridge completed 6,695 reads (27,397,632 bytes), zero writes,
and zero failures. The zero writes mean the write exit is still unit-tested only;
the bounded run ended just after fsck began.

The memory hypothesis is now measured. The guest advertised 128 MiB, began with
a 120.14 MiB post-layout free pool, and ended with 21,826 free pages (85.26 MiB),
well above its 406-page target. The last live host sample used roughly 62 MiB of
resident memory. This is not directly comparable to the later 2.98 B direct-RAM
age, but it removes the old 445 MiB static guest allocation and avoids the former
near-zero headroom at the same architectural boundary. Snapshot backing
identity/overlay state remains future work, and full NAND is the
higher-fidelity, much larger route.

### 2026-07-22: exact first raw `/dev/rmd0` boundary

A second create-only cold run extended the same guarded strategy path until the
first raw-character read. It stopped intentionally before executing `_mdevrw`
at **402,741,536** retired instructions, with no panic, undefined instruction,
or bridge failure. At the boundary:

```text
pc  c0073f94  _mdevrw entry
lr  c009920d  _spec_read+0x118
r0  09000000  /dev/rmd0
r1  ea967ef0  struct uio *
r2  00000000
```

The exact XNU32 `uio` held one 32 KiB iovec, offset zero, read direction,
segment 5, and residual `0x8000`. Before that call the strategy bridge completed
6,715 reads (27,479,552 bytes), zero writes, and zero failures. The guest had
21,187 free pages (82.76 MiB); the run's low was 21,186 pages. The immutable
firmware inputs and create-only work image were unchanged by the guard stop.

Commit `b0ec58c` replaced the audited `_mdevrw` prologue with
`svc #0xe3; bx lr` under the same exact 7E18 transaction and installed the first
bounded raw-uio bridge. Host tests covered reads, writes, XNU partial-iovec
updates, all user segment variants, the `0xc0000000` user ceiling, TTBR0/TTBR1,
legacy 1 KiB AP subpages, malformed metadata, aliases, and partial backend
failures. The next two cold runs tested that implementation rather than
assuming those host-only mappings were enough.

### 2026-07-23: run03 crossed the raw guard but fsck failed

Run03 reached `launchd` and fsck and continued to the configured
**420,000,000** retired-instruction cap, so the old `_mdevrw` guard was no longer
the boundary. The guest nevertheless reported that `/dev/rmd0 (hfs)` exited
with signal 8 (`SIGFPE`), and fsck did not complete. This was progress past the
old stop, not a successful boot: no SpringBoard frame was captured.

### 2026-07-23: run04 isolated both raw-I/O mismatches

Run04 added a per-request diagnostic and reproduced the fsck path through
405,000,000 instructions. The first failure was the offset-zero raw read:

```text
seg=5  rw=0  resid=32768  offset=0
fault=0x01001000/pa=0x00000000/fsr=0x00000807
```

This is a read from `/dev/rmd0`, but it requires a write into the user buffer.
The first page was resident and the next page needed XNU's native write-side
demand-page/COW handling; host-side page-table inspection cannot manufacture
that fault correctly.

The second failure was another 32 KiB read:

```text
offset=0x1bd30000  resid=32768  media_end=0x1bd33000
```

Only 12 KiB is inside the work image; the remaining 20 KiB is in the adjacent
allocation tail. The same two request shapes recurred in fsck's `-p` and `-fy`
passes. The closest public XNU `_mdevrw` has no logical EOF bounds check and
calls `uiomove64` once, so treating this request as an immediate `EINVAL` was
also incompatible.

The correction changes the exact four-byte patch to
`svc #0xe3; svc #0xe4` and adds `ARM_SVC_REDIRECTED`. Resident requests can
still use the bounded direct path. A translation fault redirects to the exact
Thumb `_uiomove64` entry at `0xc0128d14`, backed by one of four 128 KiB slots
keyed by the entry kernel SP and reserved below `topOfKernelData`; native XNU
then handles demand paging/COW and returns through the second SVC. A
zero-initialized, coherent 128 KiB in-memory tail preserves write-then-read
behavior beyond the media without growing either the immutable rootfs or its
work image.

### 2026-07-23: run05 cleared raw I/O and progressed through fsck

Run05 used a fresh work image and reached its **430,000,000** retired-instruction
cap with exit status 0. The serial sequence included `launchd`, then:

```text
Running fsck on the boot volume...
/dev/md0 on / (hfs, local, noatime)
```

The raw bridge completed two reads and no writes. Both reads took the native
path: two redirects, two checked completions, zero pending continuations, and
zero raw guest errors. Of the 65,536 raw bytes, 45,056 came from media and
20,480 came from the coherent guard tail. The aggregate external-md counters
were 6,901 reads (28,295,168 bytes), one 512-byte write, and zero failures;
6,899 reads and the write used the strategy bridge.

The lowest observed free-page count was 20,820 pages (81.33 MiB) at instruction
425,852,928. `_execve` recorded 11 hits and `_load_machfile` recorded 6. The
work image remained exactly 466,825,216 bytes. The authenticated kernel, device
tree, and rootfs hashes were unchanged: exact kernel patches still touched only
the loaded guest-RAM copy, and filesystem edits remained confined to the
separate work image.

The matching hosted checks at `df9dc7b` also completed successfully:
[`core-tests` run 30004015881](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30004015881)
and
[`ios-build` run 30004015807](https://github.com/j0shua-SYSON/iOS3-VM/actions/runs/30004015807).
Those workflows validate the public build and tests; run05 through run07 are
the separate private-firmware runtime evidence.

### 2026-07-23: run06 sustained the cold path to 1 B

Run06 used another fresh work image and extended the external-md cold path to
its **1,000,000,000** retired-instruction cap with exit status 0 and empty
stderr. It retained the launchd, fsck, and root-mount sequence from run05, then
printed:

```text
mDNSResponder[14] syscall_builtin_profile: mDNSResponder (seatbelt)
mDNSResponder[14] Builtin profile: mDNSResponder (seatbelt)
systemShutdown false
```

The external bridge completed 10,004 reads (40,994,304 bytes), 27 writes
(107,008 bytes), and zero failures. Strategy I/O accounted for 10,002 reads and
all 27 writes. Raw I/O remained two reads and no writes, with zero guest errors:
two native redirects, two checked completions, and zero pending continuations.
Those raw reads again split into 45,056 media bytes and 20,480 coherent-guard
bytes.

The lowest observed free-page count was 17,221 pages (67.27 MiB) at instruction
980,615,168. `_execve` remained at 11 hits while `_load_machfile` advanced to
25. The work image stayed exactly 466,825,216 bytes and the authenticated
kernel, device tree, and rootfs hashes remained unchanged.

### 2026-07-23: run07 reached a clean 2 B in userspace

Run07 used a fresh 128 MiB external-md work image and reached its
**2,000,000,000** retired-instruction cap. The exit file contained 0, stdout was
234,838 bytes, and stderr was empty. The serial record retained `launchd`, the
fsck and `/dev/md0` root-mount sequence, both `mDNSResponder[14]` Seatbelt
lines, and `systemShutdown false`.

The final PC was `0x3145ad4c` in USR mode with `CPSR 0x20000010`.
731,259,769 instructions, 36.6% of the run, retired in USR mode. The reached
probes were:

```text
_execve                    12
_load_machfile             32
_thread_bootstrap_return   92620
_unix_syscall              58166
```

The external bridge completed 12,782 reads (52,372,992 bytes), 82 writes
(325,120 bytes), and zero failures. Strategy I/O accounted for 12,780 reads and
all 82 writes. Raw I/O remained two reads and no writes, with zero guest errors:
two native redirects, two checked completions, and zero pending continuations.
Those raw reads consumed 45,056 media bytes and 20,480 coherent-guard bytes;
neither the raw media path nor the guard recorded a write.

The run ended with 13,000 free pages (50.78 MiB). Its low was 12,983 pages
(50.71 MiB) at instruction 1,836,056,576. The work image stayed exactly
466,825,216 bytes, and the source kernel, device-tree, and rootfs hashes were
unchanged.

This run cannot answer the display question. The framebuffer was disabled, and
CLCD status, interrupt mask, and scanning were all zero. Therefore run07 is
absolutely not evidence that SpringBoard started or that the real display path
works.

This chain is stronger evidence for sustained userspace and snapshot
repeatability. It is **not** evidence that SpringBoard rendered. The bounded
continuations either reached their configured caps or, for the one diagnostic
interval, stopped fail-closed on the named `UXTB16` encoding.

---

## The screen *(historical — a different boot configuration)*

Everything above is serial output. The kernel also has a graphics console, and
it can use it — but only in a run configured for it. The numbers below were
measured with `v_display = 0` and `serial=1` **dropped** from the command line,
which hands the console to the framebuffer; the standard debugging recipe at the
top of this document keeps serial and therefore paints nothing.

`initialize_screen` was reached from the very first boot that got this far, but
`boot_args.v_display` was non-zero, which makes `vcattach()` return early — so
the graphics console was never acquired and the framebuffer stayed untouched
(0 of 614,400 bytes). Setting `v_display = 0` and dropping `serial=1` from the
command line hands the console to the framebuffer, and the kernel paints its own
boot log into memory we gave it: **61,659 non-zero bytes, 20,553 lit pixels,
313 rows of text**, in XNU's own console font — 40 characters to a 320-pixel
line, so an 8-pixel cell.

Read back off the rendered image, in full, exactly as the kernel drew it
(wrapped by the console at 40 columns, not by this document):

```
iBoot version:
Seatbelt MACF policy initialized
AppleS5L8900XClockController: Dynamic Pe
rformance State Management Enabled with
max state 3
AppleS5L8900XClockController: Turbo Mode
 Supported with ratio 0x00000000 and mas
k 0x00008000
AppleBaseband: Could not find mux functi
on
IOSDIOController::init(): IOSDIOFamily-2
4.7 Dec 18 2009 01:49:48
AppleS5L8900XSDIO::init(): AppleS5L8900X
SDIO-26.0 Dec 18 2009 01:49:39
AppleS5L8900XSDIO::start(): SDIO Revisio
n 8900X
+ AppleMPVDDriver[0xc0bbd800]::init(prop
erties 0xc0bb8080)
+ AppleMPVDDriver[0xc0bbd800]::start(pro
vider 0xc0aae480)
AppleMBXDevice(0xc0bcf800): Init
AppleS5L8900XSDIO: registers @ vaddr 0xe
aa09000, paddr 0x38d00000
AppleMicron2020::start()
Registering IOCameraSensor service.
█
```

Fewer lines than the serial stream carries — the two consoles do not receive the
same messages — and it ends on a live cursor block.

The framebuffer's placement matters more than it looks. It originally sat
immediately after `boot_args`, with `topOfKernelData` advanced past it — which
moved where the kernel builds its page tables and produced a prefetch abort at
`__start+0x170`, 39,767 instructions in. Real iBoot puts the framebuffer near
the top of DRAM, so we do too, and `topOfKernelData` again describes only the
kernel's data.

What was *not active in this recorded run*: `AppleH1CLCD`, the display
controller at `/device-tree/arm-io/clcd` (physical 0x38900000, interrupt 13),
and `AppleMerlotLCD`, which needs a non-zero `lcd-panel-id` at
`/device-tree/arm-io/spi0/lcd0`. The kernel drew into its boot framebuffer but
did not drive the panel path.

To be precise about *why*, since an earlier draft of this section got it wrong:
the CLCD was already modelled — `core/src/soc/clcd.c` has tests — but this trace
left `CLCD_CTRL == 0`. `AppleH1CLCD` is not the component that programs a display
window; it adopts the first enabled window and wraps its pitch, base and geometry
in the IOSurface that becomes the screen. The current CLI can seed window 0,
patch the Merlot panel ID and capture the controller's active window. The app's
CoreGraphics view can display its synthetic guest's CLCD buffer, but it is not
wired to a shared real-guest session. Touch remains separate M5 work.

---

## Why this document exists

A boot log is the highest-density evidence an emulator can produce. Every line
above is a claim that some piece of 2009 silicon behaves the way we say it does,
made by software that has no reason to be polite about it. When the kernel says
`SDIO Revision 8900X`, it is because it read a register we implemented and
believed the answer.

The corresponding lesson is in [ROADMAP.md](ROADMAP.md): almost every bug in
this project was invisible until an unrelated fix unlocked the path that exposed
it. A boot log is how you see the path open.

How to read one when it stops making sense — the milestone table, the kext
symbolizer, snapshot/restore, and the bus reports, in the order that has actually
worked five times — is [debugging.md](debugging.md).

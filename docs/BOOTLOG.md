# Anatomy of a boot

An annotated walk through one real boot of Apple's iPhone OS 3.1.3 kernel on
iOS3-VM: from the reset vector to the last driver that starts, with the emulated
device behind each stage.

Everything here is from actual runs. Reproduce it with your own IPSW (see
[BOOT_CHAIN.md](BOOT_CHAIN.md)):

```sh
build/core/bootkernel firmware/kernel.macho \
    -d firmware/devicetree.bin \
    -c "debug=0x8 serial=1 nand-enable-adm=0" \
    -r firmware/rootfs.img -R 512 \
    -n 400000000
```

Three parts of that command line are not preferences:

- **`-R 512`.** `arm_vm_init` hardcodes `virtual_avail = 0xe0000000`, so the
  kernel's physical-linear window is exactly 512 MB. Advertising more makes
  `zone_virtual_addr` stop early-outing and index a `pv_head_table` that is
  still all zeros during `zone_bootstrap` — a null-zone dereference at ~34 M
  instructions that looks nothing like a RAM-size problem.
- **`nand-enable-adm=0`.** `AppleS5L8900XADMFMC::start` polls a NAND/DMA ready bit
  that never sets here and panics. The driver's own `probe()` honours this
  boot-arg, so it simply never matches. We boot from a RAM disk; we do not need
  NAND.
- **`-r firmware/rootfs.img`.** The real, decrypted 413 MB HFSX root filesystem
  from the IPSW, published through `/chosen/memory-map` as `RAMDisk` with `rd=md0`
  appended to the command line.

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

We do not run iBoot to hand off to the kernel; we do what iBoot does. That is a
deliberate, and stated, simplification: the interesting target is XNU, and
standing in for the bootloader is the shortest path to it that does not fake
anything the kernel can observe.

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

4,595 bytes, and every one of them from an unmodified Apple kext. Three lines are
worth naming: `AppleMobileFileIntegrity` — the code-signing enforcer — starts
cleanly; `AppleMultitouchZ2SPI` starts and asks for DMA, which is the driver half
of M5's touch input already alive and waiting for a device; and the SDIO
enumeration **fails correctly**, because nothing is soldered to the slot we
model. A driver that reports a timeout has been given a working bus.

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
413 MB HFSX volume out of Apple's own IPSW as its root.

Getting there needed two things that were not obvious. `bsd_init` calls
`IOKitInitializeTime`, which does `waitForService(resourceMatching("IORTC"),
&{tv_sec = 30})` — and `IORTC` is never published, because the PMU's real-time
clock is not modelled. Thirty seconds of guest time is a very long silence.
Patching that timeout to zero reaches `IOFindBSDRoot`. And the 512 MB `memSize`
cap above is what stops early VM init from faulting once a 413 MB disk is
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

## Stage 7 — pid 1, and where the boot stops today

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

**pid 1 executes user-mode code and makes system calls.** `_panic` is never
reached. The run does not end on a guest failure at all — it ends because *we*
stopped.

XNU does not leave VFP enabled: `_init_vfp` grants CP10/CP11 access once, and
after that the gate is `FPEXC.EN` alone, cleared per thread. So the first VFP
instruction any thread executes is *supposed* to take an Undefined exception,
which `_fleh_undef` → `_sleh_undef` → `_vfp_trap` → `_vfp_switch` turns into
"enable VFP and re-run it". That whole path now works. What stops us is the
instruction `_vfp_switch` itself uses: `0xecb10a20` decodes as
`VLDMIA r1!, {s0-s31}`, the load-multiple that restores a thread's VFP register
file — and the interpreter does not implement it. Per M1's rule an unimplemented
encoding returns `ARM_UNDEFINED` and halts the machine *at* the instruction
rather than executing something plausible. The emulator named its own gap.

Keep the scale honest: five BSD system calls and twelve Mach traps is not a
userland. Nothing has been logged by userspace, no daemon has started, and
SpringBoard needs a display controller and a panel that do not exist yet.

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

  Two independent from-scratch verifications exonerated the image *before* anything
  was changed — a UDIF verifier over all 7 `blkx` tables and every per-`blkx`
  CRC32, and an HFSX reader that checked code-directory page hashes for all 155
  signed Mach-Os and 6,731 code pages on the volume. Zero mismatches, launchd
  46/46, dyld 56/56.

  **The clinching evidence was timing.** `SHA1Transform` costs ~2,262 Thumb
  instructions per 64-byte block, so hashing 4 KB in software must cost ~145,000
  instructions. The observed `SHA1Init` → verdict interval was **14,329** — 10.1x
  too few. Software SHA-1 provably never ran, which located the bug without
  disassembling anything further.

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

What is *not* there yet: `AppleH1CLCD`, the display controller at
`/device-tree/arm-io/clcd` (physical 0x38900000, interrupt 13), and
`AppleMerlotLCD`, which needs a non-zero `lcd-panel-id` at
`/device-tree/arm-io/spi0/lcd0`. The kernel is drawing into a buffer, not
driving a panel. Wiring that buffer to the app's Metal view — and giving the
LCD kexts something to bind to — is M5 work.

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

# Anatomy of a boot

An annotated walk through one real boot of Apple's iPhone OS 3.1.3 kernel on
iOS3-VM: from the reset vector to the last driver that starts, with the emulated
device behind each stage.

Everything here is from one actual run. Reproduce it with your own IPSW (see
[BOOT_CHAIN.md](BOOT_CHAIN.md)):

```sh
build/core/bootkernel firmware/kernel.macho -d firmware/devicetree.bin -n 200000000
```

Instruction indices are counts of retired guest instructions since reset. They
are deterministic for a given build and inputs, and they move as the emulator
changes — treat them as a map, not a fingerprint.

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
| 42,008 | `_PE_init_platform` |
| 42,473 | `_DTInit` — the kernel takes the device tree we handed it |
| 42,486 | `_pe_identify_machine` — the epoch check above |
| 42,494 | `_pe_arm_get_soc_base_phys` — asks the tree where the SoC lives |
| 46,719 | first `_DTGetProperty` (858 calls before the run ends) |
| 54,439 | `_PE_parse_boot_argn` — reads the command line, 52 calls |
| 55,243 | **`_arm_vm_init`** — builds page tables and enables the MMU |

From here the kernel is executing out of its own translation regime at
0xc0000000, with TTBR0 = 0x087e0018, in tables it wrote itself, walked by our
MMU out of guest RAM through the ordinary bus — exactly as hardware would.

This stage is unforgiving in a specific way: everything before `_arm_vm_init` is
physical, everything after is virtual, and a wrong address in `boot_args` does
not fail here. It fails later, somewhere that looks unrelated.

---

## Stage 2 — finding the serial port

The kernel does not know where the UART is. It asks the device tree, and then it
maps it.

| instr | what |
|---|---|
| 99,671 | `_PE_init_kprintf` |
| 100,739 | `_serial_init` |
| 101,563 | asks `pe_arm_get_soc_base_phys` for the SoC base |
| 127,039 | finds the `uart0` node in the tree |
| 127,508 | `_ml_io_map` — maps the register page into kernel space |
| 128,699 | `serial_init` returns 1: **a working console** |
| 129,737 | `_switch_to_serial_console` |
| 132,350 | `_PE_initialize_console` |
| 132,361 | `_initialize_screen` |

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
| 10,084,711 | `_printf` entered, from `_PE_init_iokit+0x1a` |
| 10,084,798 | first `_uart_putc` — the first byte this project ever emitted |

The format string, recovered from `r0`, is `"iBoot version: %s\n"`. The argument
is empty because we did not run iBoot to leave a version string behind, so the
line comes out as `iBoot version: ` — 15 characters that took a real timer to
earn. Before the timer block was right, `mach_absolute_time()` read zero forever
and the kernel never got here at all.

Note the ten million instructions between the console being *ready* (132,350)
and the console being *used* (10,084,711). That gap is the VM system and the
zone allocator coming up: `_zcram`, `_zone_page_alloc`, `_kernel_memory_allocate`,
`_vm_page_grab`. The kernel does an enormous amount of work before it says
anything at all, which is exactly why "no output" is such a poor diagnostic and
why this project instruments call paths instead.

---

## Stage 4 — the heartbeat

| instr | what |
|---|---|
| 208,949 | **FIQ #0** — the first interrupt ever taken by this machine |
| 211,837 | `_machine_startup` |
| 213,692 | `_kernel_bootstrap` |

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
deadline before the next one had already passed. It now takes 91, costing 0.0%.

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

Two things in that table are worth dwelling on.

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

The complete list of physical pages the kernel touched outside RAM — 13 of them,
in a 200-million-instruction boot:

```
  0x3cc00000  r=2177  w=2183   uart0     modelled   first: _PE_init_kprintf+0x9c
  0x3e200000  r=6447  w=254    timer     modelled   first: _pe_arm_init_interrupts+0xba
  0x39a00000  r=7     w=21     power     modelled   (0x00-0x7f power, 0x80+ GPIO IC)
  0x38e00000  r=2     w=21     vic0      modelled   first: _pe_arm_init_interrupts+0xf8
  0x38e01000  r=0     w=11     vic1      not modelled
  0x38e02000  r=0     w=2      edgeic    not modelled
  0x3e400000  r=0     w=27     gpio      not modelled
  0x3c500000  r=1     w=6      clkrstgen not modelled
  0x3c600000  r=7     w=7      i2c0      not modelled
  0x38d00000  r=1     w=6      sdio      not modelled
  0x3d000000  r=3     w=702    unidentified (the tree puts `pke` at this offset)
  0x38100000  r=2     w=0      unidentified — no device-tree node at this offset
  0x00000000  r=1     w=0      _PE_create_console reading the framebuffer base,
                               which is zero when no framebuffer is advertised
```

---

## Stage 6 — BSD, and the wall

| instr | what |
|---|---|
| 66,655,080 | `_printf`: `"Seatbelt MACF policy initialized\n"` |
| 66,757,032 | **`_bsd_init`** |
| 78,122,355 | first `_serial_putc` (the `kprintf` path, 21 calls) |
| 132,883,590 | first data abort: DFSR 0x07, DFAR 0xea93a000 |
| 200,000,000 | run limit reached, in `_strncmp`, no panic |

Where the instructions actually go, sampled every 1,024 instructions over 744
distinct functions:

```
   22.4%  OSDictionary::getObject(OSSymbol const*)
    6.5%  _mac_file_label_init
    4.9%  OSMetaClass::checkMetaCast
    4.0%  _strncmp
    3.2%  OSObject::taggedRelease
    3.1%  OSSymbolPool::findSymbol
    3.0%  OSUnserialize
```

That profile is a portrait of IOKit matching: string interning, dictionary
lookups and metaclass casts, over and over, as the registry is built and every
prelinked personality is compared against every node. It is what a healthy 2009
kernel bringing up its driver stack looks like, and it is the strongest single
piece of evidence that the emulation is not merely *not crashing* but doing the
right work.

The 48 remaining abort sites are all the same shape: FSR 0x07 (page translation
fault) inside `IOBufferMemoryDescriptor::initWithPhysicalMask` and the kernel's
own `_fleh_dataabt`, on a marching sequence of kernel virtual addresses. The
kernel takes them and continues — `_panic` and `_Debugger` are never reached in
this boot — but "survivable and unexplained" is on the list of things to explain
rather than a thing to be pleased about.

And then the boot has nowhere left to go: there is no root filesystem, so BSD
has nothing to mount and userspace never starts. The next observable milestone
is the kernel printing `Still waiting for root device` — which sounds like a
failure and is the opposite.

---

## The screen

Everything above is serial output. The kernel also has a graphics console, and
it now uses it.

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

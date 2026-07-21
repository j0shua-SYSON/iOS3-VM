# Guest networking

How iPhone OS 3.1.3, running inside iOS3-VM, gets to the internet.

This document is a **design and a reconnaissance report**. Sections 1–4 are
findings; nothing in §5 onward is implemented yet. Every factual claim is
labelled:

- **CONFIRMED** — read directly out of the user's own `iPhone1,2_3.1.3_7E18`
  kernelcache, device tree or root filesystem, in this repository, by the
  commands shown.
- **INFERRED** — follows from something confirmed plus knowledge of how XNU of
  this vintage works. Strong, but not verified against these bytes.
- **GUESS** — plausible, unverified, and flagged so it does not quietly become
  an assumption.

Four headline conclusions, up front, because each inverts an expectation:

> **1. The 88W8686 Wi-Fi firmware is not missing. It is compiled into the
> kernelcache we already have** — `SD8686_HELPER`, 1,702 bytes, and
> `SD8686_FIRMWARE`, 121,836 bytes, sitting in `AppleMRVL868x`'s `__DATA`
> segment. There is no firmware blob anywhere on the root filesystem, and none
> is needed.
>
> **2. The S5L8900 SDIO host controller register map is no longer undocumented.**
> It is recovered in full in §2.1 from the driver's own register-dump routine,
> and cross-checked against the 10,003 reads the current boot already makes.
> Route A's "riskiest step" is now solved. Route A is still not the answer.
>
> **3. We do not need a jailbreak, and we do not need a kext.** We are iBoot and
> we build the root filesystem, so the chicken-and-egg problem is not one. And
> the guest ships **three** stock mechanisms that turn a byte pipe into an IP
> interface with no new kernel code at all: `/usr/sbin/pppd` over `pppserial`,
> `IOUserEthernet` over its `com.apple.userspace_ethernet` control socket
> (`IOEthernetControllerCreate` **is** exported by 3.1.3's `IOKit.framework`), and
> `pdp_ip0` over `AppleSerialMultiplexer`.
>
> **4. iPhone OS 3.1.3 does not use PPP for cellular data — so the obvious form
> of "route 2" is dead, and a better one takes its place.** `CommCenter` drives
> the baseband with `+CGDATA="M-RAW_IP"`, not `"PPP"`; there are zero occurrences
> of the string `ppp` in it. But `pppd` and the kernel's `pppserial` line
> discipline both ship anyway, so PPP over an *emulated UART of our own* is the
> cheapest path to guest IP on the table. That is the recommendation.

---

## 1. What we have, confirmed

### 1.1 The Wi-Fi chain, as a load map

`build/core/machoinfo.exe firmware/kernel.macho -k`, **CONFIRMED**:

```
com.apple.iokit.IONetworkingFamily     0xc04a1000 .. 0xc04b1000     65,536
com.apple.iokit.IO80211Family          0xc04b1000 .. 0xc04cb000    106,496
com.apple.iokit.IOSDIOFamily           0xc0515000 .. 0xc0522000     53,248
com.apple.nke.ppp                      0xc058d000 .. 0xc0599000     49,152
com.apple.driver.AppleMRVL868x         0xc05b8000 .. 0xc05f1000    233,472
com.apple.driver.AppleSerialMultiplexer 0xc0608000 .. 0xc0623000   110,592
com.apple.driver.AppleUSBEthernetDevice 0xc0654000 .. 0xc0659000    20,480
com.apple.iokit.IOUserEthernet         0xc06ee000 .. 0xc06f3000     20,480
com.apple.driver.AppleS5L8900XSDIO     0xc06f3000 .. 0xc06f9000     24,576
```

Matching dictionaries, read verbatim out of `__PRELINK_INFO` (**CONFIRMED**):

```
AppleS5L8900XSDIO      IOProviderClass = AppleARMIODevice
                       IONameMatch     = "sdio,s5l8900x"

AppleMRVL868x          IOProviderClass   = IOSDIOIoCardDevice
                       IOPropertyMatch   = { IOSDIOManufacturerTuple =
                                             { IOSDIOManufacturerID = 0x2DF,
                                               IOSDIOProductID      = 0x9103 } }
                       IONetworkRootType = "airport"
                       FirmwareVersion   = "9.108.5.p1-26524"

IOUserEthernet         IOProviderClass   = IOResources
                       IOResourceMatch   = IOKit
                       IOMatchCategory   = IOUserEthernetResource
                       IOUserClientClass = IOUserEthernetResourceUserClient
                       OSBundleLibraries includes AppleMobileFileIntegrity
```

### 1.2 The device tree

`/device-tree/arm-io/sdio` (**CONFIRMED**): `compatible = "sdio,s5l8900x"`,
`reg = {0x00d00000, 0x00100000}`, `interrupts = {42}`, `local-mac-address` and
`vendor-id` both **all zeros** (iBoot fills them from syscfg; we do not), and
`tx-calibration` = 40 zero bytes. With `/arm-io/ranges` mapping `0x00000000 ->
0x38000000`, the controller sits at **0x38D00000**, 1 MB window, **VIC line 42**.

The four UART nodes and what each one is wired to (**CONFIRMED**, from the child
node under each):

| Node | Child | Meaning |
|---|---|---|
| `uart0` | `iap` | accessory / iPod Accessory Protocol — also the kprintf console we already model at 0x3CC00000 |
| `uart1` | `umts` (`device_type = umts`) | **the baseband's serial line** |
| `uart3` | `bluetooth` (`bluetooth,n82`) | the CSR Bluetooth part |
| `uart4` | `debug` | debug serial |

`AppleOnboardSerial` contains `AppleOnboardSerialBSDClient`, which publishes a
**devfs node** per serial nub (`IOTTYBaseName` / `IOTTYSuffix`, and the strings
`could not allocate device node for %s` / `could not make devfs node for %s` /
`could not make kernel control socket for %s`) — **CONFIRMED**. So each of these
UARTs becomes a `/dev/tty.*` the guest's userland can open. This matters a great
deal in §6.

Free space for a device of our own (**CONFIRMED**, by enumerating every `reg`
under `/arm-io` and every `interrupts` value in the tree): **0x3F000000** is
unclaimed, and **VIC line 6** is unclaimed (as are 11, 23, 26, 29, 34, 46–62).

### 1.3 The kernel will load a kext we hand it at boot

**CONFIRMED** symbols and strings in `firmware/kernel.macho`:

```
_kxld_create_context   _kxld_link_file        __ZN6OSKext14loadExecutableEP12kxld_context
_kext_request          _kext_request_load     __ZN6OSKext17readMkext1ArchiveEP6OSDataPj
_record_startup_extensions_function           __ZN6OSKext17readMkext2ArchiveEP6OSDataPP12OSDictionaryPj
"Driver-"              "/chosen/memory-map"   "IOStartupMkextCRC"
```

`/device-tree/chosen/memory-map` has `DeviceTree {0, 0x9e60}` — and `0x9e60` is
**exactly the size of `firmware/devicetree.bin`**, which independently confirms
the entry format is `{physical address, length}` with the address left for the
bootloader — plus **sixteen spare `MemoryMapReserved-N` slots**. Property names
are fixed 32-byte fields, so renaming one to `Driver-0C000000` is an in-place
edit that resizes nothing. INFERRED: XNU of this era calls
`ml_static_ptovirt(entry->paddr)`, i.e. the stored address is physical.

### 1.4 Code signing in the guest can be switched off

**CONFIRMED** strings: `cs_enforcement_disable`, `amfi_get_out_of_my_way`,
`amfi_allow_any_signature`, `_PE_i_can_has_debugger`,
`"AMFI: Invalid signature but permitting execution"`. And
`/device-tree/chosen/debug-enabled` is a 4-byte property currently zero —
exactly the shape `dt_set_u32()` in `tools/bootkernel.c` already patches.

INFERRED (standard XNU of the era): `PE_i_can_has_debugger()` returns
`/chosen/debug-enabled`, and AMFI only honours those boot-args when it is true.
**Status: this recipe has still never been exercised in a boot.** It stays
INFERRED. `docs/activation.md` §A has the instruction-level reading.

### 1.5 What the root filesystem actually ships

A full catalog walk of all 15,894 entries (`scratchpad/rootfs/netscan.py` and
`scratchpad/ppp/ex.py`, both built on the existing from-scratch `hfs.py` reader).
**CONFIRMED**:

| Path | Size | Note |
|---|---|---|
| `/usr/sbin/pppd` | **284,608** | mode 0555, Mach-O `feedface`, cputype 12 subtype **6 (armv6)**, `MH_EXECUTE`. Stock Apple pppd of the 2.4.2 lineage, **with the tty/serial channel intact**. |
| `/System/Library/Extensions/L2TP.ppp/L2TP` | 97,488 | `PPPPluginType = Link` |
| `/System/Library/Extensions/PPTP.ppp/PPTP` | 32,480 | `PPPPluginType = Link` |
| `/System/Library/Extensions/EAP-RSA.ppp/EAP-RSA` | 20,352 | EAP |
| `/System/Library/SystemConfiguration/PPPController.bundle/PPPController` | 205,696 | configd plugin |
| `/System/Library/SystemConfiguration/IPConfiguration.bundle/IPConfiguration` | 239,760 | configd plugin |
| `/System/Library/PrivateFrameworks/CoreTelephony.framework/Support/CommCenter` | 724,208 | |
| `/private/etc/ppp`, `/private/var/log/ppp` | — | exist, **empty** |
| `/private/etc/ttys` | 1,367 | contains `tty.serial "/usr/libexec/getty serial.57600" vt100 off secure` — **off** |
| `/private/etc/fstab` | 76 | `/dev/disk0s1 / hfs ro`, `/dev/disk0s2 /private/var hfs rw,nosuid,nodev` |
| `/dev` | — | empty in the catalog; a pure devfs mount point |
| `/System/Library/Extensions/` | — | **seven entries total**, and **no `.kext` with a driver executable** |
| `/System/Library/Carrier Bundles/` | — | ~200 `*.bundle` plus ~1,200 MCC/MNC symlinks |

`pppd`'s built-in tty channel is confirmed by symbol and string, not inferred:
`_tty_channel`, `_the_channel`, `_set_up_tty`, `_connect_tty`,
`_tty_establish_ppp`, `_options_for_tty`, `_sync_serial`, plus
`"Communicate over the named device"`, `"Baud rate for serial port"`,
`"Couldn't set tty to PPP discipline: %m"`, `"Serial connection established."`,
`com.apple.nke.ppp`. **INFERRED (high confidence): `pppd <device> <baud>` needs no
plugin** — the tty channel is pppd's default channel, and the string `PPPSerial`
does not appear anywhere in the pppd binary.

Two absences worth recording because they shape the plan:

- **`PPPSerial.ppp` does not ship** — zero matches across all 15,894 entries. The
  string `"PPPSerial.ppp"` *does* exist inside `PPPController`, next to the
  serial-only options `terminalwindow`/`terminalscript`, so configd's PPP
  controller has a serial path that names a plugin which is not on the disk.
  Whether that path is unconditional could not be resolved through the PIC
  indirection — **INFERRED, unproven**. It does not matter for the recommended
  plan, which invokes `pppd` directly from launchd rather than through configd.
- **`/Library/Preferences/SystemConfiguration/preferences.plist` does not ship.**
  `/Library/Preferences` is a symlink to `../private/var/preferences`, and that
  directory is **empty**; `preferences.plist` and `NetworkInterfaces.plist` have
  zero matches in the catalog. They are created at first boot by
  `PreferencesMonitor` and `InterfaceNamer` (both compiled into `configd` — their
  bundles are Info.plist-only). So §9.1's plist must be **synthesised**, not
  copied. The entity names to build it from are confirmed in the framework's
  cstrings: `AirPort, DHCP, DNS, Ethernet, IPSec, IPv4, IPv6, L2TP, Link, Modem,
  PPP, PPPoE, PPPSerial, PPTP, Proxies, 6to4, EAPOL, OnDemand`.

### 1.6 The root filesystem is ours, and it already mounts

**CONFIRMED**: a normal boot now mounts the real HFSX root filesystem
(`firmware/rootfs.img`, 433 MB, 15,894 catalog entries). We build that image
offline; `launchd` runs from it.

**This is the answer to the chicken-and-egg question**, and it dissolves it
completely: we do not need networking to get files into the guest, because we
author the guest's disk before it boots. A daemon, a launchd plist, a
configuration file, a CA certificate, an mkext — all of them arrive by being
written into `rootfs.img` on the dev box. There is also a second, independent
delivery path (§1.3: an mkext in RAM through `/chosen/memory-map`), because we
are iBoot.

Corollary that is worth stating plainly: **"jailbreak the guest first" is not a
prerequisite for anything in this document.** Jailbreaking is what you do when
someone else controls the boot chain and the filesystem. We control both.

---

## 2. The SDIO controller side, characterised

### 2.1 The S5L8900 SDIO register map — recovered in full

`AppleS5L8900XSDIO` contains a debug routine that prints its whole register file
(`"AppleS5L8900XSDIO: Dumping SDIO Controller's register file @ vaddr 0x%08x,
paddr 0x%08x"` at `0xc06f63d8`, twenty `"   ctrl    = 0x%08lx"`-style strings
after it). The driver caches one pointer per register in its object at fixed
offsets, and `start()` initialises them from the mapped base with a chain of
`adds r3, #4` / `str r3, [r4, rN]`. Disassembling the initialiser at
**0xc06f47d0** and the printer at **0xc06f4880** and pairing slot to format
string gives the map exactly.

**CONFIRMED**, relative to `0x38D00000`:

| Off | Name | Notes |
|---|---|---|
| 0x00 | `CTRL` | |
| 0x04 | `DCTRL` | data control |
| 0x08 | `CMD` | command word — writing it starts the command |
| 0x0C | `ARGU` | 32-bit command argument |
| 0x10 | `STATE` | busy/idle state machine; driver waits for `(STATE & 0x70) == 0` |
| 0x14 | `STAC` | status-clear; driver writes the `DSTA` value back here to acknowledge |
| 0x18 | `DSTA` | **the register everything polls** |
| 0x1C | `FSTA` | FIFO status |
| 0x20 | `RESP0` | |
| 0x24 | `RESP1` | |
| 0x28 | `RESP2` | |
| 0x2C | `RESP3` | |
| 0x30 | `CLKDIV` | |
| 0x34 | `CSR` | |
| 0x38 | `IRQ` | |
| 0x3C | `IRQMASK` | |
| 0x44 | `BADDR` | DMA buffer physical address |
| 0x48 | `BLKLEN` | |
| 0x4C | `NUMBLK` | |
| 0x50 | `REMBLK` | remaining blocks |
| 0x54 | *(unnamed)* | mapped only when `sdio-version` says revision **8900X** |
| 0x6C | *(unnamed)* | mapped unconditionally, and again on 8900X |

0x40 is not mapped by the driver. (This map agrees with the register names
openiBoot uses for the same block. It was derived here from Apple's binary
alone, so it does not depend on that project for correctness — and openiBoot is
GPL, so it may be consulted for facts and never copied. See §10.)

### 2.2 `sendCommand`, instruction by instruction

`AppleS5L8900XSDIO::sendCommand` is at roughly `0xc06f5380..0xc06f5644`,
**Thumb** (the whole kext is Thumb — worth knowing before anyone disassembles
it as ARM and concludes it is garbage). It does, in order (**CONFIRMED**):

1. Build the command word: the SD command index in the low bits, `| 0x40` when
   the command carries a data phase, and a **response-type field in bits
   [18:16]** taken from a 0x33-entry jump table indexed by `cmd - 3` at
   `0xc06f53aa`. The decoded table is delightfully clean — **the field is just
   the SD response number**:

   | Command | bits [18:16] | Response |
   |---|---|---|
   | CMD3 | 6 | R6 (published RCA) |
   | CMD5 | 4 | R4 (IO OCR) |
   | CMD7 | 1 | R1/R1b |
   | CMD52, CMD53 | 5 | R5 |
   | everything else | 0 | none |

2. `*ARGU = arg`, then `*CMD = cmdword`.
3. Poll `STATE` until `(STATE & 0x70) == 0`, up to **10000** iterations, else
   `"Timeout waiting for command idle indication"`.
4. Poll `DSTA` until **bit 4** is set, up to **10000** iterations, else
   `"Timeout waiting for CMDRDY indication"`. On each pass it also checks four
   error bits — **bit 15**, **bit 16**, **bit 17** and **bit 18** — and maps each
   to a different string in `"CMD%d Failure - %s"`. **Bit 18 is explicitly
   tolerated when the command is CMD5**, which is the tell that bit 18 is the
   response-CRC error: R4 carries no CRC, so a CRC complaint on CMD5 is expected.
5. Write the `DSTA` value back to `STAC` to acknowledge.
6. Copy `RESP0..RESP3` into the command object.

### 2.3 Why the current boot fails, exactly

`core/src/soc/` has **no SDIO model at all** — not even a stub. 0x38D00000 falls
through to the generic unmapped-MMIO path in `core/src/soc/machine.c`
(`note_unmapped`), so reads return zero and writes are counted. **CONFIRMED.**

Therefore `DSTA` reads zero forever, bit 4 never sets, and the loop in step 4
runs its full 10000 iterations before printing `"Timeout waiting for CMDRDY"`.
`docs/BOOTLOG.md` records **`0x38d00000 r=10003 w=10`**. 10000 loop reads, plus
the loop's final read, plus the `STATE` read, plus one more — the arithmetic
closes. That is a stronger confirmation of the register map than any single
disassembly: the count the emulator measured is the count the decoded loop
predicts.

### 2.4 What `enumerateSlot` needs to see

`IOSDIOController::enumerateSlot` (`IOSDIOFamily`, around `0xc0516858`) is a
textbook SDIO bring-up (**CONFIRMED**):

1. **CMD5** with argument 0 → R4 in `RESP0`. Four accessors pick it apart, and
   each is a four-instruction leaf function we can read exactly:
   - `IOSDIOCommand` +? at `0xc051c4ee`: **OCR = `RESP0 & 0x000FFFFF`**
   - at `0xc051c4fc`: **number of I/O functions = `(RESP0 >> 28) & 7`**
   - at `0xc051c50a`: **memory present = bit 27**
   - at `0xc051c512`: **ready (`C`) = bit 31**
2. If `C` is clear it logs `"SDIO device not ready, trying CMD5 again"`,
   `IOSleep(1)`, and retries — **up to 100 times**, then gives up with
   `"Timed out waiting for card to become ready"` and sets a `Cmd5IoRdyTimeout`
   registry property.
3. **CMD3** → R6, the card's published RCA.
4. **CMD7** with that RCA → select the card. It prints all four response words.
5. `"Found SDIO I/O device. Function count(%d), memory(%d)"`, then it creates an
   `IOSDIOIoCardDevice` nub, starts it and attaches it.
6. The nub then does **CMD52** reads of CCCR, walks the **CIS tuple chain**
   (`parseFn0CIS`, `readCIS`, `getCISData`), and publishes
   `IOSDIOManufacturerTuple = { IOSDIOManufacturerID, IOSDIOProductID }` plus
   `IOSDIOManufacturer` / `IOSDIOProduct` / `IOSDIOProductInfo0` / `…Info1`
   from the version tuple. That published dictionary is what `AppleMRVL868x`'s
   `IOPropertyMatch` tests.
7. Then function enable, block-size programming through FBR, high-speed and bus
   width negotiation, and **CMD53** block transfers driven through
   `BADDR`/`BLKLEN`/`NUMBLK`/`REMBLK`.

**So the minimum register behaviour to get past CMD5 and reach the CIS is small
and completely specified:** on a write to `CMD`, latch the argument, produce the
right response words in `RESP0..3` per the [18:16] type field, set `DSTA` bit 4,
leave `STATE & 0x70` clear, and clear the bits that get written to `STAC`. For
CMD5, return `RESP0` with bit 31 set, bits 30:28 = 1 (one I/O function), bit 27
clear, and any non-zero OCR. That is perhaps 300 lines of C, and it is the
*whole* of "make Apple's SDIO stack believe a card is there".

---

## 3. The driver side: `AppleMRVL868x`

### 3.1 The firmware blobs are in the kernelcache — this is the decisive fact

The kext logs `"AppleMRVL868x: SD8686_FIRMWARE is %lu bytes."` and
`"AppleMRVL868x: SD8686_HELPER is %lu bytes."` — the names of **compile-time
symbols**, i.e. the images are static arrays linked into the driver. The literal
pool at `0xc05bcdc0` carries the two lengths next to the two format strings:

```
  c05bcdc0  c05cb4c8   -> "AppleMRVL868x: SD8686_FIRMWARE is %lu bytes.\n"
  c05bcdc4  0001dbec   =  121,836
  c05bcdc8  c05cb4f8   -> "AppleMRVL868x: SD8686_HELPER is %lu bytes.\n"
  c05bcdcc  000006a6   =   1,702
```

The kext's own Mach-O header (prelinked kexts keep theirs) puts `__DATA/__data`
at **0xc05d2000, 0x1e6e4 bytes** — 124,644, just big enough for both images plus
about a kilobyte of ordinary globals. And the pointers line up:

- **`SD8686_HELPER` at 0xc05d2000**, 1,702 bytes. Referenced from `0xc05bbab8`.
  Its first words are `ea000003 / e59f0050 / ee010f10 / e3e00000` — an ARM branch
  over a vector table followed by a `MCR p15` — exactly what a boot helper for
  the 8686's internal ARM core looks like.
- **`SD8686_FIRMWARE` at 0xc05d29a0**, 121,836 bytes, referenced from
  `0xc05c5c80`, ending at `0xc05f058c` — and `0xc05f058c` is itself referenced as
  the next `__data` global. The arithmetic closes exactly.

**CONFIRMED, and searched for exhaustively on the other side:** the root
filesystem contains **no** Marvell firmware. The catalog walk of all 15,894
entries returns **zero** matches for `mrvl`, `marvell`, `8686` or `868x` in any
path. `/usr/share/firmware/` contains only `multitouch/Common.mtprops` and
`multitouch/iPhone.mtprops`. `/System/Library/Extensions/` contains exactly seven
entries — `AppleMultitouchSPI.kext`, `IOHIDFamily.kext`, `IOUSBDeviceFamily.kext`,
`IOUSBFamily.kext`, `EAP-RSA.ppp`, `L2TP.ppp`, `PPTP.ppp` — and **no
`AppleMRVL868x.kext`** and no other `.kext` bundle at all. (There *is* a
`com.apple.wifiFirmwareLoader` launch daemon, which is a red herring: with the
images statically linked into the kext there is nothing on disk for it to load.)

So: we have the firmware. It just was never a file.

### 3.2 What the driver does with it

**CONFIRMED** from strings: a two-stage download. `loadHelperImage` pushes the
1,702-byte helper over CMD53 writes to function 1; the helper then reports how
many bytes of the main image it wants next (`"No transmit length from helper
image."`), the driver feeds the main image in helper-chosen chunks, and the
helper reports CRC status (`"Helper image reports CRC error in Main Program
image."`) and finally a ready magic (`"Timed out wating for ready status from
helper firmware"` — Apple's typo, not mine). Before that it reads and programs
function block sizes (`"Failed to read blocksize LSB/MSB"`, `"Failed to set the
FN%u block size to %u"`) and enables the function.

Afterwards it runs the full Marvell/libertas host command protocol. The command
names are in the binary verbatim and match the public Marvell naming exactly:

```
HostCmd_CMD_802_11_MAC_CONTROL      HostCmd_CMD_802_11_SCAN
HostCmd_CMD_802_11_ASSOCIATE        HostCmd_CMD_802_11_DEAUTHENTICATE
HostCmd_CMD_802_11_KEY_MATERIAL     HostCmd_CMD_802_11_SET_WEP
HostCmd_CMD_802_11_BG_SCAN_QUERY    HostCmd_CMD_802_11_RSSI
HostCmd_CMD_802_11_RF_TX_POWER      HostCmd_CMD_802_11_TPC_CFG
HostCmd_CMD_802_11_GET_TSF          HostCmd_CMD_802_11_GET_LOG
HostCmd_CMD_802_11_INACTIVITY_TIMEOUT(_EXT)
HostCmd_CMD_802_11_HOST_SLEEP_CFG   HostCmd_CMD_802_11_HOST_SLEEP_ACTIVATE
HostCmd_CMD_802_11_POWER_ADAPT_CFG_EXT
HostCmd_CMD_802_11_RATE_ADAPT_RATESET
HostCmd_CMD_802_11_TX_RATE_QUERY    HostCmd_CMD_802_11_AD_HOC_START
HostCmd_CMD_MAC_REG_ACCESS          HostCmd_CMD_BBP_REG_ACCESS
HostCmd_CMD_MEM_ACCESS              HostCmd_CMD_MAC_MULTICAST_ADR
HostCmd_CMD_SDIO_PULL_CTRL          HostCmd_CMD_ROBUST_BT_COEX
MAC_EVENT_DISASSOCIATED             MAC_EVENT_BG_SCAN_REPORT
```

…plus a full association state machine (`fTryingToAssociate`,
`fAssociateBeaconIndex`, `fAssocCandidateBSS`, beacon container, roam logic) and
IEEE status-code mapping.

### 3.3 Three further obstacles specific to Route A, found in `start()`

`AppleMRVL868x::start()` funnels every failure into one exit at `0xc05bca76`
that logs and returns `false`. Reached from, among others (**CONFIRMED**):

- `"AppleMRVL868x: Unable to get my hardware address."` and
  `"AppleMRVL868x: MAC Address is all 00's."` — and our device tree's
  `local-mac-address` on `/arm-io/sdio` **is** all zeros. We would have to patch
  it. Cheap, but it is on the critical path and nobody had noticed.
- `"AppleMRVL868x: No Calibration Data in device tree."` /
  `"Invalid calibration data in device tree!"` — `tx-calibration` is 40 zero
  bytes in our tree. Whether zeros count as invalid is not yet established
  (GUESS: they are rejected).
- `"AppleMRVL868x: Failed to get AppleBaseband."` — guarded by a check on a
  driver field being 1 or 2 (INFERRED: a board/feature revision), and reached via
  `waitForService(AppleBaseband)` at `0xc05bc81e`. If that gate is taken and
  `AppleBaseband` never registers, `start()` **blocks**. The current boot already
  logs `AppleBaseband: Could not find mux function`.

---

## 4. The routes, ranked

Five routes are on the table. Two of them are new since the last revision of this
document, and one of the new ones is the recommendation.

| | Route | New guest kernel code | New guest userland | Emulator work | Verdict |
|---|---|---|---|---|---|
| **A** | Emulate the real 88W8686 over SDIO | none | none | SDIO controller + card + Marvell host protocol + a synthetic 802.11 world | **Deferred.** Months. §4.1 |
| **B** | Paravirtual NIC + a kext we write | **a kext** | none | ~600 lines | Good, but gated on the armv6 kext toolchain. §4.2 |
| **C** | `IOUserEthernet` + a userland daemon | none | a daemon | transport + NAT | **Strong fallback.** §4.3 |
| **D** | **PPP over an emulated UART** | **none** | **none — `pppd` ships** | a UART pipe + a PPP peer | **Recommended.** §4.4 |
| **E** | `pdp_ip0` — emulate the raw-IP baseband | none | none — `CommCenter` does it | mux + AT-command modem | The *native* path. §4.5 |

### 4.1 Route A — the real Marvell part

Honest reassessment now that the binaries have been read.

**What got easier.** The two things this document previously called the riskiest
are done or nearly done. The register map (§2.1) is recovered. The command
sequence, the response-type encoding, the poll bits and the CMD5 acceptance
criteria (§2.2, §2.4) are all read off the instructions. The firmware blobs exist
and we know where they are (§3.1). Getting from "nothing on the bus" to
`AppleMRVL868x` matching and beginning its firmware download is now a *bounded*
job: an SDIO host-controller model, an SDIO card model, a CIS with MANFID
0x02DF/0x9103, and a sink for the firmware download. Call it **3–5 weeks** for
one person, and every step of it is testable off the phone.

**What did not get easier, and is the actual blocker.** After the download the
driver expects to talk to *running firmware*. We have the firmware image, but it
is ARM code for the 8686's own processor driving the 8686's own MAC and radio —
emulating that is a second SoC, not a device model. So we would have to
**reimplement the Marvell host protocol ourselves**: forty-odd commands with TLV
payloads, an event channel, a scan that returns beacons, an association that
succeeds, key material, RSSI, power-save, background scan. And there is no radio
behind it, so all of that would be a *synthesised* 802.11 world.

**Is the protocol documented?** Partly, and honestly not enough. Linux's
`libertas` driver implements the same command set with the same names, so the
command IDs, structure layouts and TLVs are *checkable facts* from a real
implementation — but `libertas` is GPL and may only be consulted, never copied
(§10). What is **not** confirmable from anywhere is the behaviour Apple's driver
depends on from firmware **9.108.5.p1-26524** specifically: which events it
expects unsolicited, which TLVs it requires echoed, the exact `capinfo` and
status codes it maps. Per the project rule — *trap what you don't implement,
never guess* — **we would be inventing that, and inventing it is disallowed.**

So: **Route A is deferred, not rejected.** The parts that are confirmable
(§2, §3.1) are worth building anyway, because they are a milestone you can watch
happen: `IOSDIOController` finding a card, walking the CIS, and `AppleMRVL868x`
matching and downloading 121,836 bytes of Apple's own firmware into our sink.
That is a genuinely impressive artefact and it costs weeks, not months. The
*months* begin the moment the firmware is supposed to answer. Add to that the
three `start()` obstacles in §3.3. Route A is the trophy, and the trophy buys
exactly two things: the Settings → Wi-Fi pane, and the status-bar glyph.

### 4.2 Route B — paravirtual NIC plus a kext we write

A 4 KB MMIO window of our own design with descriptor rings, plus an
`IOEthernetController` subclass we build and inject as a boot-time mkext.
Everything about it is under our control and unit-testable in milliseconds, which
is the property this repo is built around. The full device design is preserved in
§7 because it remains the right answer *if* the kext toolchain works.

The risk is one thing and it is not code signing: **producing an `armv6`
`MH_KEXT_BUNDLE` whose C++ ABI and `OSObject` vtable layout match a kernel built
by GCC 4.2 in 2009.** Apple never shipped a `Kernel.framework` in the iPhoneOS
SDK and never released the ARM XNU sources. If clang's ARM vtable layout diverges
we get a kext that links cleanly and branches into space. Everything else in this
document is ordinary work; that is the only step that might simply not be
possible.

Routes C, D and E all exist to **remove that risk entirely**, and they do.

### 4.3 Route C — `IOUserEthernet`, no kernel code at all

`IOUserEthernet.kext` is prelinked, and its internals are now read rather than
assumed (**CONFIRMED**, strings in `0xc06ee000..0xc06f3000`):

```
IOUserEthernetResourceUserClient      createController
com.apple.networking.ethernet.user-access      "IOUserEthernet: %s is not entitled"
Cookie   HardwareAddress   MaxPacketSize   MinPacketSize
IOUserEthernetController      virtual bool IOUserEthernetController::setLinkState(bool)
com.apple.userspace_ethernet          en_register      "%s - ctl_register failed: %d"
```

That is a complete, kext-free path to a **real `enN` Ethernet interface**: open
the `IOUserEthernetResource` user client, call `createController` with a
`HardwareAddress` and packet-size limits, and then exchange raw Ethernet frames
on a `PF_SYSTEM` / `SYSPROTO_CONTROL` socket named `com.apple.userspace_ethernet`.

And the userland half is no longer a hope. The previous revision of this document
listed "`IOEthernetControllerCreate` may not exist in 3.1.3's `IOKit.framework`"
as risk #4, to be checked before betting on this route. **It has been checked, and
it exists.** Parsing `IOKit.framework`'s symbol table out of the guest's dyld
shared cache (image base `0x314A0000`, 1,661 symbols) gives, all with
`n_type = 0x0F` (`N_SECT|N_EXT`) — **CONFIRMED**:

```
0x314a0680  _IOEthernetControllerCreate
0x314a0514  _IOEthernetControllerGetIONetworkInterfaceObject
0x314a063c  _IOEthernetControllerGetTypeID
0x314a0408  _IOEthernetControllerReadPacket
0x314a038c  _IOEthernetControllerWritePacket
0x314a04d4  _IOEthernetControllerSetLinkStatus
0x314a0304  _IOEthernetControllerRegisterPacketAvailableCallback
0x314a02ec  _IOEthernetControllerRegisterEnableCallback
0x314a02f8  _IOEthernetControllerRegisterDisableCallback
0x314a0350  _IOEthernetControllerScheduleWithRunLoop
0x314a0310  _IOEthernetControllerUnscheduleFromRunLoop
```

with the matching cstrings `IOUserEthernetResource`,
`com.apple.userspace_ethernet`, `HardwareAddress`, `IOEthernetController`,
`IONetworkInterface`, `IOMIGMachPort` in the same image. So a guest daemon gets a
real `enN` in about twenty lines of CoreFoundation, and does not even need to
drive the user client by hand.

The one remaining gate is the entitlement `com.apple.networking.ethernet.user-access`;
`IOUserEthernet`'s `OSBundleLibraries` lists `AppleMobileFileIntegrity`,
confirming AMFI performs the check. INFERRED: an ad-hoc/`ldid` signature carrying
the entitlement is honoured once the signature is not validated — the ordinary
fake-signing path, and the repo already uses `ldid` for the host app. Belt and
braces: §1.4's boot-args.

What Route C still needs is a **host↔guest transport for the frames**, which is
§6 — the same UART Route D uses.

### 4.4 Route D — PPP over an emulated UART ⭐ **recommended**

This is the route the brief asked to be taken seriously rather than dismissed,
and having read the binaries it is not just viable, it is the cheapest thing on
the list by a wide margin.

First, a correction to the premise, and it is a hard one. **iPhone OS 3.1.3 does
not use PPP for cellular data on this device — not anywhere, not at all.**
`grep -i ppp` over all 7,777 strings in `CommCenter` returns **zero hits**. No
`pppd`, no `/dev/tty`, no LCP, no IPCP, no PAP, no CHAP. What CommCenter does
instead is spelled out by its own format strings (**CONFIRMED**):

```
  +cmux=0,0,0,%d               GSM 07.10 multiplexer on the baseband link
  ioctl(ASMIOCNEWDLCI)         open a DLCI channel on /dev/mux.*
  +cgdcont=%d,"IP","%s"        define the PDP context with the APN
  +xgauth=%d,1,"%s","%s"       Infineon-proprietary auth   (not PAP/CHAP)
  +xdns=%d,1                   Infineon-proprietary DNS    (not IPCP)
  +cgact=1,%d                  activate the PDP context
  +cgdata="M-RAW_IP",%d        <<<< enter RAW IP data mode, not "PPP"
  +cgpaddr=%d                  read back the assigned address
  +xdns?                       read back the DNS servers
  ioctl(SIOCPROTOATTACH) / SIOCAIFADDR / SIOCSIFMTU
```

`+CGDATA="M-RAW_IP"` is the whole answer: on a standard modem that argument would
be `"PPP"`. Here IP datagrams are framed directly on the mux channel with no
HDLC, and CommCenter plumbs the address onto the interface itself. Corroborating:
`"Cellular WAN: pdp_ip%d"`, `pdp_ctl`, `"PDP %d change states %s->%s"`. On the
kernel side `AppleSerialMultiplexer` supplies `MuxNetworkInterface`,
`pdpInterfaceCreate`, `pdpActivate` and the `ASMIOCPDP*` ioctls, attaching a
raw-IP `ifnet` (`ifnet_attach`, `proto_register_plumber(PF_INET)`, and an
`"Urecognized version field 0x%04X"` check on the IP version nibble).

The carrier bundles agree. `/System/Library/Carrier Bundles/ATT_US.bundle/carrier.plist`
(2,660 bytes, binary plist) configures data with an `apns` array of
`{apn, username, password, type-mask, signature}` — and **contains no dial
string, no `ConnectionScript`, no `ATD*99#`, and no PPP or auth-protocol key at
all**, exactly matching the `+CGDCONT`/`+XGAUTH` pair above. (Each entry carries a
128-byte RSA `signature`, so hand-editing an APN fails validation:
`"Found garbage for the APN settings in the managed profile."`)

Anyone planning "fake a modem on the baseband link and let the OS speak PPP at
it" would have spent a week discovering that. It is dead. (See Route E, which is
what the discovery is actually worth.)

**But PPP itself is very much present, on both sides, and complete.**

*Kernel:* `com.apple.nke.ppp` is prelinked at `0xc058d000` and contains a full
**`pppserial` line discipline** (**CONFIRMED** — `pppserial_init`,
`pppserial_open`, `pppserial_attach`, `pppserial_input`, `pppserial_ioctl` with
`TIOCSETD`/`TIOCGETD`/`PPPIOCGCHAN`, `pppserial_lk_ioctl` with
`PPPIOCSASYNCMAP`/`PPPIOCSRASYNCMAP`/`PPPIOCSMRU`, async-HDLC framing errors
`"garbage received: 0x%x (need 0xFF)"`, `"missing UI (0x3), got 0x%x"`,
`"bad fcs %x, pkt len %d"`, FCS checking, `ppp%d` interfaces, VJ header
compression, `ppp_ip_attach`/`ppp_ipv6_attach`). Classic PPP-over-a-tty, in the
kernel, already loaded.

*Userland:* **`/usr/sbin/pppd` ships, 284,608 bytes, armv6, with its built-in tty
channel intact** — §1.5. It needs no plugin to drive a serial device, and the
missing `PPPSerial.ppp` is configd's problem, not ours, because we invoke `pppd`
from a launchd job rather than through `PPPController`.

*Devices:* §1.2 established that `AppleOnboardSerialBSDClient` publishes a devfs
node per UART nub, and there are four nubs already in the device tree.

So the plan is:

1. **Model one of the S5L8900 UARTs as a byte pipe to the host.** We already
   model `uart0` (`core/src/soc/uart.c`, 1.8 KB) as the kprintf console, so the
   register behaviour is known and the second instance is close to free. The
   candidate is a UART whose child node names a device we will never emulate —
   `uart3`/`bluetooth` is the obvious one; `uart1`/`umts` is the semantically
   "right" one (it is literally the modem line) but risks contention with
   baseband drivers. Either way **no device-tree change is required**, because
   all four nodes already exist.
2. **Guest side: run `pppd` against that tty**, from a launchd plist we put on
   `rootfs.img`. Zero new guest code, zero kexts, zero code-signing exposure —
   `pppd` is Apple's own signed binary.
3. **Host side: implement the PPP peer.** LCP + IPCP, no authentication, assign
   the guest an address, then hand IP packets to the same user-space NAT that
   every other route needs anyway (§8).

The cost of the PPP peer is **LCP (RFC 1661) + IPCP (RFC 1332) + HDLC-async
framing (RFC 1662)**, all fully and publicly documented, all with test vectors,
and small: call it **1,000–1,500 lines** and a week, most of which is
option-negotiation bookkeeping. Compare with: a kext of unknown feasibility, or
an SDIO stack, or a Marvell protocol we would have to invent.

**Honest limitations of D.** PPP gives a point-to-point interface, not Ethernet:
no ARP, no DHCP, no broadcast domain. It is also not automatically a "network
service" to SystemConfiguration, so `SCNetworkReachability` will not report
reachable until §9.1's preferences plist exists — and that plist does not ship
(§1.5), so it has to be synthesised. On iPhone OS a PPP service *is* a
first-class SC concept (`PPPController`, `kSCValNetInterfaceSubTypePPPSerial` is
in the framework's entity list), so this is far more tractable than it would be
for `utun`, but it is real work and it is where N6 will be spent. Throughput is
bounded by our UART model — but the UART is emulated, there is no real baud rate,
and we can make it as fast as we are willing to make it.

If any of that turns sour, **Route D degrades into Route C over the identical
transport**: the same emulated UART becomes the frame pipe for an
`IOUserEthernet` daemon, we get a real `enN` with ARP and DHCP, and nothing built
for D is wasted except the PPP peer itself.

### 4.5 Route E — be the baseband, and let `CommCenter` build `pdp_ip0`

The discovery in §4.4 has a constructive side, and it is the most *authentic*
non-Route-A option: **do not fake a modem badly, fake this exact modem.** The AT
sequence is not guesswork — CommCenter's own format strings spell out every
command, and the carrier bundle supplies the APN. `AppleSerialMultiplexer` is
already alive in our boot (`mux::start: created new mux (18) for spi-baseband`),
it publishes `/dev/mux.*` nodes, and its ioctl surface is fully named
(**CONFIRMED**):

```
ASMIOCCHECKVERSION  ASMIOCCONFIG  ASMIOCENGAGE  ASMIOCNEWDLCI  ASMIOCNEWDLCIEXT
ASMIOCPDPIFCREATE   ASMIOCPDPACTIVATE   ASMIOCPDPDEACTIVATE   ASMIOCPDPIFDESTROY
```

Emulate enough of an Infineon baseband — `+CMUX`, `+CGDCONT`, `+XGAUTH`, `+XDNS`,
`+CGACT`, `+CGDATA="M-RAW_IP"`, `+CGPADDR`, `+XDNS?` — behind the mux transport,
and **`CommCenter` creates `pdp_ip0` itself, assigns the address itself, and sets
the DNS itself.** No new kernel code, no new userland, and the wire format on the
data channel is **plain IP packets**. This is the only route besides A that
produces an interface the OS already understands as *cellular data*, which very
plausibly means a real status-bar glyph.

Why it is not the recommendation: the mux's own framing over its adapter
(`RS232Adapter` / `SPIAdapter` / `H5Adapter`) is undocumented and would have to be
reverse-engineered out of a 110 KB kext, and CommCenter has to be brought far
enough along its state machine — SIM, registration, carrier bundle selection — to
get to the data part at all. That is a bigger, less predictable surface than
LCP/IPCP. Keep it as the ambitious fallback, and as the thing to build if we ever
want the guest to *look* like it is on a network rather than merely be on one.

A cheaper variant exists and is worth remembering: a small helper of our own on
the root filesystem can issue `ASMIOCNEWDLCI` + `ASMIOCPDPIFCREATE` +
`ASMIOCPDPACTIVATE` directly, cutting CommCenter out entirely and leaving only
the mux transport to emulate.

---

## 5. Recommendation

> **Build Route D. Model a second S5L8900 UART as a host byte pipe, run the
> guest's own `/usr/sbin/pppd` over it from a launchd job on our root filesystem,
> terminate PPP in the emulator, and NAT out through ordinary unprivileged
> sockets.**
>
> **Fallback, sharing the same transport and the same NAT: Route C** —
> `IOUserEthernet` driven by a small daemon on our root filesystem, which needs
> no kext either.
>
> **Keep Route B** (our own kext + `vmnet0`) as the answer if we ever want a real
> Ethernet interface with better throughput than a UART, and **keep Route A** as
> the eventual trophy that lights up the Wi-Fi UI. Build Route A's SDIO half
> anyway — §2 makes it a bounded, watchable milestone.

The reasoning, in order of weight:

1. **It removes the only step in this project that might be impossible.** Routes
   C, D and E all need zero new guest kernel code. The armv6 kext toolchain risk
   (§4.2) does not merely get mitigated; it stops existing.
2. **Both halves are already on disk.** `pppd` with its tty channel in userland
   (§1.5), `pppserial` in the kernel (§4.4). We write nothing that runs in the
   guest except a launchd plist.
3. **Every component we do write is documented and testable off the phone.**
   RFC 1661 / 1332 / 1662, with published test vectors, against a UART model we
   have already written once.
4. **The transport is shared with the fallback.** If PPP disappoints, the same
   emulated UART carries Ethernet frames for Route C — whose userland half is now
   also confirmed present (§4.3). Low-regret first move.
5. **The chicken-and-egg problem does not exist** (§1.6). We author `rootfs.img`.
6. It forecloses nothing. Nothing in it touches `/arm-io/sdio`.

The honest cost, stated once: **Settings → Wi-Fi will show nothing, and there
will be no Wi-Fi glyph in the status bar.** Only Route A buys those back. Apps
will still work, because apps test reachability, not the glyph.

---

## 6. The transport: an S5L8900 UART as a host pipe

**Guest-visible:** nothing new. `uart0`, `uart1`, `uart3`, `uart4` are all already
in `firmware/devicetree.bin` (§1.2), all already match `AppleS5L8900XSerial`, and
`AppleOnboardSerialBSDClient` already publishes a devfs node for each. Every boot
we have already run prints `AppleS5L8900XSerial: Identified Serial Port on ARM
Device=uart0/uart1/uart3/uart4`.

**Emulator-visible:** a second instance of the model in `core/src/soc/uart.c`,
with RX fed from a host-side queue and TX drained into one. The FIFO-empty /
data-available status bits already have to be right for `uart0` to work as the
console, so the incremental work is the RX path and an interrupt.

**Which UART.** `uart3`/`bluetooth` is the safest (we will never model the CSR
part, and `AppleBluetooth` is only 12 KB and currently inert). `uart1`/`umts` is
the semantically honest one — it is the modem line, and "present a fake modem on
the serial link" then becomes literally true. Decide by experiment: bring up
`uart3` first because nothing contends for it, and keep `uart1` in reserve.

**The node names to expect.** `/dev` is an empty directory in the catalog — pure
devfs — so every node is created at runtime. `CommCenter`'s strings name
`/dev/uart.umts` and `/dev/uart.debug` (**CONFIRMED**), which is the
`AppleOnboardSerialBSDClient` naming applied to the device tree's child node
names, so `uart3`/`bluetooth` should surface as `/dev/uart.bluetooth`. Confirm by
reading the boot log rather than by assuming; and note `/private/etc/ttys` ships a
`tty.serial` getty entry that is **off**, so nothing will contend for the port.

**Flow control and speed.** There is no real baud rate. The model should drain TX
as fast as the guest writes and raise RX interrupts as fast as the guest drains,
with a bounded queue that applies back-pressure by leaving the TX-ready bit
clear. The CPU thread must never block on the network.

---

## 7. If we do build our own NIC: `vmnet0`

Preserved from the previous design, unchanged, because it remains correct and it
is what Route B would build. It is **not** on the recommended path.

### 7.1 Register map

A single 4 KB window at physical **0x3F000000** (`/arm-io` child offset
`0x07000000`). All registers 32-bit, little-endian, word-aligned. Reads of
unimplemented offsets return zero; writes are counted, not swallowed, in keeping
with the rest of `core/src/soc/`.

| Off | Name | Access | Meaning |
|---|---|---|---|
| 0x000 | `ID` | R | `0x544E4D56` (`"VMNT"`). A driver that does not see this must refuse to attach. |
| 0x004 | `VERSION` | R | `1`. Bumped on any incompatible ring change. |
| 0x008 | `FEATURES` | R | bit0 link-state reporting. Reserved bits read zero. |
| 0x00C | `IRQ_STATUS` | R/W1C | bit0 `RX_AVAIL`, bit1 `TX_DONE`, bit2 `LINK_CHANGE`. |
| 0x010 | `IRQ_MASK` | R/W | Same bit positions; 1 = allowed to assert the line. |
| 0x014 | `CONTROL` | R/W | bit0 `ENABLE`, bit1 `PROMISC`, bit2 `ALLMULTI`, bit31 `RESET`. |
| 0x018 | `STATUS` | R | bit0 `LINK_UP`. |
| 0x01C | `MTU` | R | `1500`. |
| 0x020 | `MAC_LO` | R | MAC bytes 0–3, byte 0 in bits 7:0. |
| 0x024 | `MAC_HI` | R | MAC bytes 4–5 in bits 15:0. |
| 0x028 | `RX_RING_PA` | R/W | Physical base of the RX descriptor ring. 16-byte aligned. |
| 0x02C | `RX_RING_LEN` | R/W | Descriptor count. Power of two, 8…256. |
| 0x030 | `RX_HEAD` | R | Device's producer index. |
| 0x034 | `RX_TAIL` | R/W | Driver's "buffers posted up to here" index. |
| 0x038 | `TX_RING_PA` | R/W | Physical base of the TX ring. |
| 0x03C | `TX_RING_LEN` | R/W | Descriptor count. |
| 0x040 | `TX_HEAD` | R/W | Driver's producer index. **Writing this is the kick.** |
| 0x044 | `TX_TAIL` | R | Device's consumer index. |
| 0x048–0x054 | `STAT_*` | R | rx pkt, tx pkt, rx drop, tx drop. |

Ring registers are writable only while `CONTROL.ENABLE == 0`.

### 7.2 Descriptors

16 bytes, little-endian, in guest **physical** memory:

```
  +0  u32  buf_pa     physical address of the frame buffer
  +4  u16  buf_len    RX: capacity.  TX: frame length.
  +6  u16  pkt_len    RX: bytes written.  TX: must be 0.
  +8  u32  flags      bit0 EOP (always 1 in v1 — no scatter/gather)
  +12 u32  reserved   must be zero; the device rejects the descriptor otherwise
```

### 7.3 Ring protocol

Single-producer/single-consumer head/tail pairs in the Intel style, with no OWN
bit — the indices alone define ownership, which removes a class of race.

**Transmit.** Driver writes the descriptor at `TX_HEAD % LEN`, `DSB`, writes
`TX_HEAD + 1`; that MMIO write is the kick. The device consumes from `TX_TAIL` to
`TX_HEAD`, advances `TX_TAIL`, sets `TX_DONE`.

**Receive.** Descriptors in `[RX_HEAD, RX_TAIL)` are posted buffers. On arrival
the device fills `RX_HEAD % LEN`, writes `pkt_len`, advances `RX_HEAD`, sets
`RX_AVAIL`. If `RX_HEAD == RX_TAIL` it **drops and counts — it never blocks the
CPU thread.**

**Cache coherency.** iOS3-VM does not model the data cache, so no maintenance is
required today. The driver should nevertheless use `IOBufferMemoryDescriptor`
with `prepare()`/`complete()`, so the day a cache model appears it is already
right.

### 7.4 Interrupt and device-tree node

**Level-triggered** on **VIC0 line 6**, asserted while `(IRQ_STATUS & IRQ_MASK)
!= 0`. Level, not edge, because `core/src/soc/vic.c` models levels and because an
edge device that re-raises during the handler loses the interrupt.

The node goes under `/arm-io` with `compatible = "vmnet,ios3vm"`,
`reg = {0x07000000, 0x00001000}`, `interrupts = {6}`, a copied
`interrupt-parent`, and a `local-mac-address`. `AppleARMPlatform` turns every
`/arm-io` child into an `AppleARMIODevice` nub matched on its `compatible`
string — **CONFIRMED** by `AppleS5L8900XSDIO` matching `"sdio,s5l8900x"` and
`AppleSynopsysOTG2` matching `"usb-otg,s5l8900x"`.

**Tooling gap:** `tools/bootkernel.c` can overwrite properties in place but
cannot add a node; it would need a `dt_add_child()`. Contained work, but real.
Note that **Routes C, D and E need none of this** — that is much of their appeal.

### 7.5 MAC address

Locally-administered unicast (first octet `0x02`), low octets from the guest ECID
or generated once, and **persisted next to the NAND image** — a MAC that changes
every launch means a new DHCP lease, a new host ARP entry and a different `en0`
identity in the guest's SystemConfiguration preferences every boot.

---

## 8. The host side: getting packets out of an iOS 15 app

### 8.1 The constraint, and why it turns out not to bite

A sandboxed iOS app cannot open raw sockets or create a TUN device. The device is
jailbroken, so more is *possible* — but the recommendation is to use none of it,
and that is a considered choice rather than a concession.

| Approach | Privilege needed | Verdict |
|---|---|---|
| **User-space TCP/UDP NAT** | **none** | **Recommended.** Works as `mobile`, sandboxed, and on a phone that is not jailbroken at all. |
| `utun` on the host | root (INFERRED) | Rejected. Also needs `net.inet.ip.forwarding` and `pfctl` rules — it reconfigures the whole phone's networking to give one app a network. |
| `SOCK_RAW` / `/dev/bpf` L2 bridging | root | Rejected. Consumer APs refuse a second MAC, and Apple's Wi-Fi driver will not usefully go promiscuous. |

**Guest networking is not blocked on any entitlement, privilege, or jailbreak
question — on either side.** It is ordinary application code.

### 8.2 The NAT

The emulator terminates guest IP and acts as the gateway. Same idea as QEMU's
SLIRP, but **we write our own**: QEMU is GPL, and even if `libslirp`'s licence
permits reuse (GUESS — described as BSD-style; verify before anyone links it), a
dependency contradicts the zero-dependency property that makes `core/` test in
under a second. Estimated **2–3k lines of C11** in `core/src/net/`.

```
  guest 10.0.2.15/24      gateway/NAT 10.0.2.2      DNS 10.0.2.3
```

- **IPv4**, with reassembly refused rather than half-implemented.
- **ICMP echo** → `socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)`, unprivileged on
  Darwin (INFERRED for iOS). If not, answer echoes locally.
- **UDP** → `SOCK_DGRAM`, keyed on the 4-tuple with an idle timer.
- **TCP** → one host `SOCK_STREAM` per guest flow. The only genuinely hard part:
  a real, if small, TCP state machine facing the guest — sequence/ack tracking, a
  receive window, retransmission, delayed ACK, MSS clamped to 1460, correct
  FIN/RST translation. A half-correct TCP produces stalls that look like emulator
  bugs, which is the failure mode this project is worst placed to debug. Hence
  milestone **N0**: written and tested before any guest exists.
- **DNS**, answered locally by parsing the query and calling the host's
  `getaddrinfo()` — this inherits the host's resolver configuration instead of
  forwarding port 53, which a captive network may block.
- **ARP and DHCP** — needed only for the Ethernet-shaped routes (B and C). Route
  D is point-to-point and needs neither; IPCP hands out the address.

**Not implemented:** inbound connections except explicit forwards, IPv6, and
anything that is not TCP/UDP/ICMP.

### 8.3 Threading

The NAT runs on its own thread with a `kqueue` loop. Guest→host frames cross on a
lock-free SPSC queue drained by that thread; host→guest frames are pushed into
the transport under a small mutex. **The CPU thread must never block on the
network** — a full queue drops and counts.

---

## 9. What the guest needs beyond a working link

### 9.1 Address configuration

1. **Bootstrap (instant, verifiable).** For Route D, `pppd` and IPCP do this by
   themselves. For Routes B/C, a launchd job on our root filesystem running
   `ifconfig en0 inet 10.0.2.15 …`, `route add default 10.0.2.2`, and writing
   `nameserver 10.0.2.3` into `/etc/resolv.conf`.
2. **Real (needed for the OS to believe it).** Write a
   `preferences.plist` into `/private/var/preferences/SystemConfiguration/`
   (`/Library/Preferences` is a symlink to `../private/var/preferences`, and the
   directory is empty — **CONFIRMED**, §1.5) describing a network service on the
   interface with `InterfaceSubType = PPPSerial`. Only then does `configd`
   publish the service into the dynamic store and make `SCNetworkReachability`
   report reachable — which is what CFNetwork, Safari, Mail and every third-party
   app test before they will attempt a connection.

   **Nothing ships to copy the schema from** — `preferences.plist` and
   `NetworkInterfaces.plist` have zero matches in the catalog, because
   `PreferencesMonitor` and `InterfaceNamer` (both compiled into `configd`)
   create them at first boot. So this plist must be synthesised from the entity
   names in `SystemConfiguration.framework`'s cstrings: `AirPort, DHCP, DNS,
   Ethernet, IPSec, IPv4, IPv6, L2TP, Link, Modem, PPP, PPPoE, PPPSerial, PPTP,
   Proxies, 6to4, EAPOL, OnDemand`. INFERRED: the structure is close to but not
   identical to macOS 10.6's. This is the one part of N6 with real unknowns.

### 9.2 What the UI will and will not show

- **Settings → Wi-Fi: empty.** It is driven by `Apple80211` against
  `IO80211Family`. Only Route A changes this.
- **Status bar: no Wi-Fi glyph, no EDGE glyph.** Same reason. (Route E is the one
  non-A route that would plausibly produce a data glyph, since `pdp_ip0` is the
  real cellular interface and `CommCenter` would be the thing bringing it up.)
- **`SCNetworkReachability`: reachable.** So apps that gate on reachability —
  nearly all of them — will proceed.

### 9.3 TLS in 2026, honestly

iPhone OS 3.1.3's SecureTransport does SSLv3 and TLS 1.0 with RSA key exchange
and RC4/3DES/AES-CBC-SHA1; no ECDHE, no AES-GCM, no TLS 1.2, probably no SNI
(INFERRED from the era — read the suite list out of the guest's
`Security.framework` before relying on it). Its root store is frozen in 2010 and
lacks ISRG Root X1 and everything since.

- **Plain HTTP: works.** That is the target for the "on the internet" milestone.
- **HTTPS to the modern internet: essentially everything fails**, three
  independent ways at once — protocol version, cipher suite, untrusted root. Any
  one is fatal.
- **Apple's own 2010 services are gone.** Activation, the iTunes and App Stores,
  Maps' tile API, the YouTube app's API, the Weather feed — dead endpoints or
  dead protocols. Say this out loud in the README rather than letting people
  discover it.

**The fix that works** is a TLS-terminating proxy, in two escalating forms:
(1) an **explicit proxy** inside the emulator app, pointed at through CFNetwork's
proxy settings in the same SystemConfiguration preferences — ten lines of plist
plus `CONNECT` handling; (2) **transparent interception**, where the NAT redirects
port 443 to a local listener speaking TLS 1.0/RC4 down to the guest and TLS 1.3
up to the internet, with certificates signed by a CA we generate once and add to
the guest's trust store — which is a file copy, because we build the root
filesystem. That makes *unmodified* guest apps work over HTTPS.

Even then, Safari 3.x has no modern JavaScript or CSS. Expect a deliberately
period-appropriate demo page. That is not a bug in the emulator.

---

## 10. Staged plan

Each stage ends in something you can watch. Everything before N3 needs no phone.

| | Stage | Observable | Est. |
|---|---|---|---|
| **N0** | **Host NAT, standalone.** `core/src/net/` plus a `tools/nettest` harness that feeds hand-built IP packets in and prints what comes back. | `ctest` green on: an ICMP echo reply, a DNS answer, and a full synthetic TCP handshake fetching `GET /` from a real host. **No guest, no phone, sub-second.** | 1–2 wk |
| **N1** | **UART transport.** A second `core/src/soc/uart.c` instance wired to a host queue, plus `core/tests/`. | Bytes written by a bare-metal payload to `uart3` appear on the host queue and vice versa. | 2–3 d |
| **N2** | **PPP peer.** HDLC-async framing, LCP, IPCP, in `core/src/net/ppp.c`. | Unit tests replay recorded LCP/IPCP exchanges; the peer negotiates and assigns 10.0.2.15. | 1 wk |
| **N3** | ⚠️ **The milestone that proves the route.** A launchd plist on `rootfs.img` running `/usr/sbin/pppd <tty> noauth nodetach local` (no plugin needed — §1.5). | `ppp0` appears in the guest and `ifconfig` shows an address our peer assigned. | 2–3 d |
| **N4** | **ICMP and TCP end to end.** | `ping 10.0.2.2` replies; a static test binary on our rootfs completes a handshake to a real internet host and echoes bytes. | 1 wk |
| **N5** | **A real HTTP fetch.** | `GET http://example.com/` from the guest prints HTML to the console. **This is "the guest is on the internet."** | days |
| **N6** | **The OS believes it.** SystemConfiguration preferences, reachability. | **MobileSafari renders a live web page.** A 2009 phone's browser, on a 2015 phone, loading today's internet. The shareable artefact. | 1–2 wk |
| **N7** | **TLS.** Proxy plus a guest CA. | Safari over HTTPS. | 1–2 wk |
| **S1** | *(parallel, optional)* **SDIO card model.** §2 in code: host controller registers, CMD5/3/7, CCCR, a CIS with MANFID 0x02DF/0x9103. | `IOSDIOController` logs `Found SDIO I/O device`, walks the CIS, and `AppleMRVL868x` matches and starts downloading 121,836 bytes of Apple's own firmware into our sink. | 3–5 wk |
| **N8** | *(trophy, deferred)* Route A proper. | Settings → Wi-Fi lists a network and the glyph appears. | months |

**N0, N1 and N2 are not blocked on M5.** They can be finished today against the
current milestone state. N3 needs `launchd` to run jobs, which is M5's business.

---

## 11. Risks, in order of how likely they are to hurt

1. **Our TCP against the guest's TCP.** Subtle bugs masquerade as emulator bugs,
   which is the failure mode this project is worst placed to debug. N0 exists
   precisely to stop that, and it is first on purpose.
2. **The SystemConfiguration story.** `preferences.plist` does not ship (§1.5), so
   N6 must synthesise one from the framework's entity names rather than adapt a
   shipped file. Worst case N6 slips and we live on N3–N5's plain `ppp0` for
   longer, which is enough for "the guest is on the internet" but not enough for
   Safari.
3. **`pppd` may refuse a tty it does not like** — modem control lines, carrier
   detect, `PPPIOCGCHAN`. Mitigable with `local`, `nodetach`, `nocrtscts` and by
   making the UART model report carrier permanently asserted, but it is the kind
   of thing that costs a day of log-reading. The emulator is the ideal debugger
   for it: we see every register access.
4. **The entitlement gate on `IOUserEthernet`**
   (`com.apple.networking.ethernet.user-access`) if we fall back to Route C.
   INFERRED that a fake signature carrying the entitlement is honoured once
   enforcement is off. Cheap to test early, and worth testing early precisely
   because it is the fallback.
5. **The armv6 kext toolchain** — demoted from #1 to #5, because no recommended
   route needs it. It only returns if we choose Route B.
6. **Route A's `start()` obstacles** (§3.3): the all-zero `local-mac-address`, the
   all-zero `tx-calibration`, and the conditional `waitForService(AppleBaseband)`.
   Only relevant to S1/N8, but they sit on that critical path and were previously
   unknown.

---

## 12. Licensing

This project is MIT and ships no Apple code. That constrains what may be
consulted and what may be copied:

- **QEMU, `qemu-ios`, openiBoot, and Linux's `libertas` are GPL.** They may be
  consulted for *facts* — register offsets, protocol sequences, documented
  constants — and never copied. The register map in §2.1 was derived
  independently from Apple's binary specifically so that it does not rest on
  openiBoot. Route A would lean on `libertas` for facts about the 88W8686 command
  set; the implementation must be ours.
- **The PPP RFCs (1661, 1662, 1332) and the SDIO Simplified Specification are
  public**, and that is a large part of why Route D is the recommendation: it can
  be built entirely from open specification.
- **`libslirp` is described as BSD-licensed** (GUESS — verify before linking).
  Irrelevant in practice: we write our own NAT, because a dependency would break
  the zero-dependency property of `core/`.
- **Apple's `IONetworkingFamily` headers are APSL-2.0.** A *build-time*
  dependency for Route B only, used the way an SDK is used. Not vendored here.
- **`SD8686_HELPER` and `SD8686_FIRMWARE` are Marvell/Apple binaries** inside the
  user's own kernelcache. They are never extracted into this repository, never
  redistributed, and are only ever read out of firmware the user already owns.
- The mkext container format is documented in xnu's `libkern/libkern/mkext.h`;
  a writer would be a clean reimplementation reusing the LZSS compressor already
  in `core/src/firmware/lzss.c`.

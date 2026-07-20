# Guest networking

How iPhone OS 3.1.3, running inside iOS3-VM, gets to the internet.

This document is a **design**, not a report on working code. Nothing here is
implemented yet. Every factual claim is labelled:

- **CONFIRMED** — read directly out of the user's own `iPhone1,2_3.1.3_7E18`
  kernelcache or device tree, in this repository, by the commands shown.
- **INFERRED** — follows from something confirmed plus knowledge of how XNU of
  this vintage works. Strong, but not verified against these bytes.
- **GUESS** — plausible, unverified, and flagged so it does not quietly become
  an assumption.

The headline conclusion, up front, because it inverts the obvious expectation:

> **The jailbreak on the host buys us nothing for networking, and guest code
> signing is not the thing that kills this.** The host can reach the internet
> from a completely unprivileged, unentitled, sandboxed app. The guest kernel
> ships with the machinery to load a kext we build, and with boot-args that
> switch code signing off. The genuinely risky part is much more boring: getting
> a modern compiler to emit an `armv6` kext whose C++ vtables match a kernel
> built by GCC 4.2 in 2009.

---

## 1. What we already know

Everything in this section was extracted from the firmware in `firmware/` and is
**CONFIRMED**.

### 1.1 The guest already has an entire networking stack

Bundle identifiers in the prelinked kernelcache (`strings firmware/kernel.macho`,
`__PRELINK_INFO` plist):

| Kext | Why it matters |
|---|---|
| `com.apple.iokit.IONetworkingFamily` (1.8) | `IOEthernetController`, `IONetworkStack`, BSD `ifnet` glue. Already there. |
| `com.apple.iokit.IO80211Family` (211.1) | The 802.11 layer. `OSBundleRequired = Network-Root`. |
| `com.apple.driver.AppleMRVL868x` (1.1.0) | **The stock Marvell 88W8686 Wi-Fi driver, prelinked.** |
| `com.apple.iokit.IOSDIOFamily` | Generic SDIO: CMD5/CMD52/CMD53, CCCR, CIS tuples. |
| `com.apple.driver.AppleS5L8900XSDIO` | The SoC's SDIO host controller driver. |
| `com.apple.iokit.IOUserEthernet` | Lets **userspace** create an Ethernet interface. |
| `com.apple.driver.AppleUSBEthernetDevice` | Ethernet-over-USB (the tethering path). |
| `com.apple.driver.AppleUSBDeviceMux` | usbmux. |
| `com.apple.driver.DiskImages.RAMBackingStore` | RAM disk root. |

The exact matching dictionaries matter, so they are recorded verbatim:

```
AppleS5L8900XSDIO      IOProviderClass = AppleARMIODevice
                       IONameMatch     = "sdio,s5l8900x"

AppleMRVL868x          IOProviderClass = IOSDIOIoCardDevice
                       IOPropertyMatch = { IOSDIOManufacturerTuple =
                                           { IOSDIOManufacturerID = 0x2DF,
                                             IOSDIOProductID      = 0x9103 } }
                       IONetworkRootType = "airport"
                       FirmwareVersion   = "9.108.5.p1-26524"

IOUserEthernet         IOProviderClass   = IOResources
                       IOResourceMatch   = IOKit
                       IOUserClientClass = IOUserEthernetResourceUserClient
```

`AppleS5L8900XSDIO`'s `IONameMatch` on `AppleARMIODevice` is the pattern our own
driver will copy, so it is worth noting that this is not invention — it is how
every S5L8900 driver in this kernelcache binds.

### 1.2 The device tree already describes the Wi-Fi hardware

From `firmware/devicetree.bin`, node `/device-tree/arm-io/sdio`:

```
compatible        = "sdio,s5l8900x"
device_type       = "sdio"
reg               = { 0x00d00000, 0x00100000 }
interrupts        = { 42 }
vendor-id         = 0            (iBoot fills this from syscfg)
local-mac-address = 00:00:00:00:00:00   (iBoot fills this from syscfg)
no-sdio-devices   = 0
sdio-version      = 0
tx-calibration    = <40 bytes of zeros>
```

`/arm-io/ranges` is `{0x00000000 -> 0x38000000, size 0x08000000}` and
`{0x10000000 -> 0x18000000, size 0x10000000}`, so the SDIO controller's physical
base is **0x38D00000**, 1 MB window, **VIC line 42**. The same arithmetic
reproduces `uart0` at 0x3CC00000, which is the value already in `core/include/soc.h`
— so the range decoding is right.

The presence of `local-mac-address` on the `sdio` node is the tell that this node
*is* the Wi-Fi part: the S5L8900 has exactly one SDIO slot and the 88W8686 is
soldered to it.

### 1.3 Free space in the memory map

Enumerating every `reg` under `/arm-io` and every `interrupts` value in the whole
tree gives us somewhere safe to put a device of our own:

- **0x3F000000** (arm-io child offset `0x07000000`) is unclaimed. The nearest
  neighbours are `chipid` at 0x3E500000 and `edram` at 0x48000000.
- **VIC line 6** is unclaimed (as are 11, 23, 26, 29, 34, and 46–62). Line 6 sits
  in VIC0, which is the controller `core/src/soc/vic.c` already models.

### 1.4 The kernel will load a kext we hand it at boot

Symbols and strings **CONFIRMED** in `firmware/kernel.macho`:

```
_kxld_create_context   _kxld_link_file        __ZN6OSKext14loadExecutableEP12kxld_context
_kext_request          _kext_request_load     __ZN6OSKext17readMkext1ArchiveEP6OSDataPj
_record_startup_extensions_function           __ZN6OSKext17readMkext2ArchiveEP6OSDataPP12OSDictionaryPj
_add_from_mkext_function                      __ZN12KLDBootstrap21readStartupExtensionsEv
"Driver-"              "/chosen/memory-map"   "IOStartupMkextCRC"
```

This is the classic BootX startup-extension path: at boot XNU walks
`/chosen/memory-map`, and for every property whose name begins with `Driver-` it
treats the `{address, length}` pair as an **mkext archive** in memory, unpacks it,
and links each kext with `kxld` against the running kernel. Both mkext1 and mkext2
readers are present.

And the device tree has room for it. `/device-tree/chosen/memory-map` contains:

```
DeviceTree            len=8   { 0x00000000, 0x00009e60 }
MemoryMapReserved-0 .. -15    len=8   { 0, 0 }        (sixteen of them)
```

Two things fall out of that. First, `0x9e60` is **40544 bytes — exactly the size
of `firmware/devicetree.bin`**, which independently confirms the entry format is
`{physical address, length}` with the address left at zero for the bootloader to
fill. Second, there are **sixteen spare slots**, and device-tree property names
are fixed 32-byte fields, so renaming `MemoryMapReserved-4` to `Driver-0C000000`
(15 characters) or to `RAMDisk` (7) is an **in-place edit that resizes nothing**.
`tools/bootkernel.c` already patches this tree in place; it needs one new helper
(rename a property) rather than a new tree writer.

INFERRED: XNU of this era does `ml_static_ptovirt(entry->paddr)` on that address,
i.e. the stored address is **physical**. This follows from the xnu-1456 sources
and from the field being named `paddr`; it is not proven against these bytes.

### 1.5 Code signing in the guest can be switched off with a boot-arg

**CONFIRMED** strings in this kernel:

```
"%s: cs_enforcement disabled by boot-arg"    cs_enforcement_disable
_cs_enforcement_disable                       amfi_get_out_of_my_way
_PE_i_can_has_debugger                        amfi_allow_any_signature
"AMFI: Invalid signature but permitting execution"
"AMFI: unrestricted debugging is enabled."    amfi_unrestrict_task_for_pid
```

And `/device-tree/chosen/debug-enabled` is a **4-byte property currently set to
zero** — precisely the shape `dt_set_u32()` in `tools/bootkernel.c` already
patches.

INFERRED (this is standard XNU behaviour of the era, and both halves of it are
confirmed present here): `PE_i_can_has_debugger()` returns the value of
`/chosen/debug-enabled`, and AMFI only honours `cs_enforcement_disable` and
`amfi_get_out_of_my_way` when it returns true. So the recipe is:

```
device tree:  /chosen/debug-enabled = 1
boot-args:    cs_enforcement_disable=1 amfi_get_out_of_my_way=1
```

We are iBoot. We write both. This is not a patch to Apple's kernel — it is the
configuration Apple's kernel already reads, which is why development devices
existed.

> **This is the answer to "will code signing kill it?" — and the answer is no,
> twice over.** For the recommended route the question does not even arise,
> because a kext delivered as an in-memory mkext never touches a vnode, and
> `AMFI_vnode_check_signature` is a MAC hook on vnode execution. I searched this
> kernel for any kext-signature enforcement string and found none — but *absence
> of a string is not proof*, so the boot-args above stay in the plan as belt and
> braces, and verifying that a self-built kext loads is milestone **N2**, placed
> early on purpose.

---

## 2. The three routes

### Route A — emulate the real Marvell 88W8686 over SDIO

Emulate the S5L8900 SDIO host controller and a card behind it that identifies as
Marvell 0x02DF/0x9103, so `AppleS5L8900XSDIO` → `IOSDIOFamily` →
`AppleMRVL868x` → `IO80211Family` all bind **completely unmodified**.

**What we would have to build**

1. The S5L8900 SDIO *host controller* register map. **This is not documented
   anywhere public** — the device tree says `sdio,s5l8900x`, not SDHCI, and
   openiBoot does not implement it. It would have to be reverse-engineered by
   tracing which registers `AppleS5L8900XSDIO` touches, which is exactly the
   method that produced the timer model in `core/include/soc.h`. Encouragingly
   the driver is only `0x6000` bytes.
2. The SD/SDIO card protocol: CMD5 (`IO_SEND_OP_COND`), CMD3, CMD7, CMD52
   (`IO_RW_DIRECT`), CMD53 (`IO_RW_EXTENDED`), the CCCR register block, function
   basic registers, and a CIS tuple chain carrying MANFID 0x02DF/0x9103. The
   SDIO Simplified Specification is public, so this part is mechanical.
   The driver's own log strings tell us the whole sequence it expects
   (`"CMD5 response: Number of I/O functions: %u"`, `"Failed to read CCCR
   Interrupt Pending register"`, `"@%lu - Manufacturer tuple 0x%02X, size %u"`).
3. The 88W8686's function-1 register block plus its **two-stage firmware
   download**: a helper image, then a main image fed in chunks whose sizes the
   helper asks for, then a ready magic. Confirmed by the driver's strings
   (`"No transmit length from helper image"`, `"Timed out waiting for ready
   status from helper firmware"`, `"downloaded %u bytes of firmware data"`).
   The firmware blobs live in the kext bundle on the root filesystem
   (INFERRED — the kext is 232 KB and the strings reference named images).
4. The Marvell **command interface**: `GET_HW_SPEC`, `MAC_CONTROL`, `SNMP_MIB`,
   `802_11_SCAN`, `ASSOCIATE`, `RF_TX_POWER`, power-save configuration, and
   their TLVs. Each one we answer wrongly is a bail-out. This is the large,
   open-ended part.
5. Only then the data path — which is mercifully easy, because the 8686 is a
   *full-MAC* part: the host exchanges **802.3 frames** with a small Marvell
   TX/RX header, not raw 802.11. We never need an 802.11 PHY or MAC.

**Verdict.** This is the trophy version and the only route that makes
**Settings → Wi-Fi and the status-bar glyph actually work**. It is also, honestly,
a multi-month specialist project with a real chance of stalling at step 1 on an
undocumented register map. Given that guest internet is stated as critical, it
cannot be the first attempt. It is **deferred, not rejected** — everything in
§1.1–1.2 is groundwork for it, and the recommended route deliberately does not
foreclose it.

### Route B — a paravirtual NIC plus a kext we write

A small MMIO device of our own design with descriptor rings, plus an
`IOEthernetController` subclass we build and inject.

**Cost:** we write both sides, so both are exactly as complex as we choose. The
emulator side is a few hundred lines and is unit-testable on the dev box in
milliseconds, which is the property this repo is built around. The guest side is
a ~600-line kext.

**Risk:** it needs a toolchain that emits an `armv6` `MH_KEXT_BUNDLE`, and it
needs the kernel to accept it. §1.4 and §1.5 say the kernel will. The toolchain
is the real problem — see §7.

### Route C — the alternatives worth naming

**C1. Ethernet over the emulated USB device controller.**
`AppleUSBEthernetDevice` is prelinked and matches `IOUSBDeviceInterface` with
`USBDeviceFunction = "AppleUSBEthernet"`. Guest side would be 100% stock. But it
requires emulating the Synopsys DWC OTG controller in *device* mode, and the
function is only published when tethering is enabled — which on iPhone OS 3 is
gated by a carrier bundle, and which sets up the phone as the *gateway*
(it expects to route onward to `pdp_ip0`, which does not exist here). Too many
gates we do not control, in exchange for solving a problem route B solves in a
day. **Rejected**, though the DWC OTG model will be wanted eventually for DFU
and restore.

**C2. `IOUserEthernet` — a userspace daemon, no kext at all.** ⭐ *Named fallback.*
`IOUserEthernet.kext` is **CONFIRMED prelinked**, exposing
`IOUserEthernetResourceUserClient` on `IOResources`, gated by the entitlement
`com.apple.networking.ethernet.user-access` (**CONFIRMED** string
`"IOUserEthernet: %s is not entitled"`). A userland daemon on our root filesystem,
ad-hoc-signed with that entitlement, can create a real Ethernet interface with
**zero kernel-mode code**.

It would reach the emulator through a **hypercall**, because we are the CPU: the
interpreter can treat an otherwise-undefined encoding (say `MCR p7, 3, Rd, c0, c0, 0`)
as a call gate at any privilege level, with r0 = opcode, r1 = guest *virtual*
buffer address, r2 = length — and translate r1 through the MMU we already have.
No device, no device-tree node, no driver, no kxld.

This is a genuinely attractive plan B, and its cost profile is inverted from
route B's: an `armv6` *userland* binary is far easier to produce than a kext.
Its load-bearing assumption is **INFERRED, not confirmed**: that
`IOEthernetControllerCreate()` exists in iPhone OS 3.1.3's `IOKit.framework`.
The plausible 2009 consumer is Bluetooth PAN tethering, which shipped in 3.0.
**Verify by dumping the guest's dyld shared cache before betting on this.**

**C3. `utun` in the guest.** `com.apple.net.utun_control` is **CONFIRMED** present
in this kernel, and utun of this vintage had no entitlement check (INFERRED). A
guest daemon over the same hypercall could get a point-to-point IP interface with
even less ceremony than C2 — no Ethernet, no ARP. Weaker, though: a `utunN`
interface is invisible to SystemConfiguration's idea of a "network service", so
`SCNetworkReachability` and therefore most apps will not believe there is a
network. Good enough to prove packets flow; not good enough to make Safari work.
**Keep as a debugging shortcut, not as the design.**

---

## 3. Recommendation

> **Build Route B: `vmnet0`, a paravirtual Ethernet NIC, with the guest driver
> delivered as a boot-time mkext through `/chosen/memory-map`, and host egress
> through a user-space TCP/UDP NAT that requires no entitlements whatsoever.**
>
> Keep **C2 (`IOUserEthernet` + hypercall)** as the named fallback if the kext
> toolchain proves intractable, and keep **Route A** as the eventual trophy that
> lights up the Wi-Fi UI.

Why:

- It is the only route where **every piece is under our control and testable off
  the device**, which is the property that has made this project work so far.
- The two things that *look* like blockers are not. The host needs no
  privilege (§5). The guest kernel already carries kxld, the mkext loader, and
  the boot-args that disable code signing (§1.4, §1.5).
- It does not foreclose Route A. Nothing in this design touches the `sdio` node.
- The honest cost is stated in §6: **Settings → Wi-Fi will show nothing and the
  status bar will show no Wi-Fi glyph.** Apps will still work, because apps test
  reachability, not the glyph.

---

## 4. The guest-visible device: `vmnet0`

### 4.1 Register map

A single 4 KB window at physical **0x3F000000** (`/arm-io` child offset
`0x07000000`). All registers are 32-bit, little-endian, word-aligned. Reads of
unimplemented offsets return zero; writes are counted, not swallowed, in keeping
with the rest of `core/src/soc/`.

| Off | Name | Access | Meaning |
|---|---|---|---|
| 0x000 | `ID` | R | `0x544E4D56` (`"VMNT"`). A driver that does not see this must refuse to attach. |
| 0x004 | `VERSION` | R | `1`. Bumped on any incompatible ring change. |
| 0x008 | `FEATURES` | R | bit0 link-state reporting. Reserved bits read zero. |
| 0x00C | `IRQ_STATUS` | R/W1C | bit0 `RX_AVAIL`, bit1 `TX_DONE`, bit2 `LINK_CHANGE`. Write 1 to clear. |
| 0x010 | `IRQ_MASK` | R/W | Same bit positions; 1 = allowed to assert the line. |
| 0x014 | `CONTROL` | R/W | bit0 `ENABLE`, bit1 `PROMISC`, bit2 `ALLMULTI`, bit31 `RESET` (write-1, self-clearing, returns every register and both rings to reset state). |
| 0x018 | `STATUS` | R | bit0 `LINK_UP`. |
| 0x01C | `MTU` | R | `1500`. |
| 0x020 | `MAC_LO` | R | MAC bytes 0–3, byte 0 in bits 7:0. |
| 0x024 | `MAC_HI` | R | MAC bytes 4–5 in bits 15:0; upper half zero. |
| 0x028 | `RX_RING_PA` | R/W | Physical base of the RX descriptor ring. 16-byte aligned. |
| 0x02C | `RX_RING_LEN` | R/W | Descriptor count. Power of two, 8…256. |
| 0x030 | `RX_HEAD` | R | Device's producer index. |
| 0x034 | `RX_TAIL` | R/W | Driver's "buffers are posted up to here" index. |
| 0x038 | `TX_RING_PA` | R/W | Physical base of the TX ring. |
| 0x03C | `TX_RING_LEN` | R/W | Descriptor count. |
| 0x040 | `TX_HEAD` | R/W | Driver's producer index. **Writing this is the kick.** |
| 0x044 | `TX_TAIL` | R | Device's consumer index. |
| 0x048 | `STAT_RX_PKT` | R | Frames delivered to the guest. |
| 0x04C | `STAT_TX_PKT` | R | Frames accepted from the guest. |
| 0x050 | `STAT_RX_DROP` | R | Frames dropped because no RX buffer was posted. |
| 0x054 | `STAT_TX_DROP` | R | Frames dropped as malformed or oversized. |

Ring registers are only writable while `CONTROL.ENABLE == 0`. Setting `ENABLE`
with a ring address of zero or a non-power-of-two length leaves `ENABLE` clear
and is visible as an unmapped-style diagnostic rather than as silent death.

### 4.2 Descriptors

16 bytes, little-endian, in guest **physical** memory:

```
  +0  u32  buf_pa     physical address of the frame buffer
  +4  u16  buf_len    RX: capacity of the buffer.  TX: length of the frame.
  +6  u16  pkt_len    RX: bytes the device wrote.  TX: must be 0.
  +8  u32  flags      bit0 EOP (always 1 in v1 — no scatter/gather)
  +12 u32  reserved   must be written zero; the device rejects the descriptor
                      otherwise, so a future field cannot be silently ignored
```

Buffers must not straddle the end of RAM; the device bounds-checks in 64-bit
arithmetic for the same reason `core/src/soc/machine.c` does.

### 4.3 Ring protocol

Both rings are single-producer/single-consumer with a head/tail pair, in the
Intel style. There is no OWN bit; the indices alone define ownership, which
removes a whole class of race.

**Transmit.** The driver writes a descriptor at `TX_HEAD % TX_RING_LEN`, issues a
`DSB`, then writes `TX_HEAD + 1`. That MMIO write is the kick: the device
consumes descriptors from `TX_TAIL` up to `TX_HEAD`, hands each frame to the host
NAT, advances `TX_TAIL`, and sets `IRQ_STATUS.TX_DONE`. The ring is empty when
`TX_HEAD == TX_TAIL` and full when `TX_HEAD - TX_TAIL == TX_RING_LEN`.

**Receive.** Descriptors in `[RX_HEAD, RX_TAIL)` are empty buffers the driver has
posted. When a frame arrives the device fills the descriptor at
`RX_HEAD % RX_RING_LEN`, writes `pkt_len`, advances `RX_HEAD`, and sets
`IRQ_STATUS.RX_AVAIL`. The driver keeps its own consumer cursor in software,
consumes everything up to `RX_HEAD`, and then advances `RX_TAIL` to give the
buffers back. If `RX_HEAD == RX_TAIL` when a frame arrives, the device drops it
and increments `STAT_RX_DROP` — **it never blocks the CPU thread.**

**Cache coherency, stated plainly.** iOS3-VM does not model the data cache, so no
maintenance is required for correctness *today*. The driver should nonetheless
use `IOBufferMemoryDescriptor` with `prepare()`/`complete()` around every buffer,
so that the day a cache model appears the driver is already right. This is
exactly the sort of "works by accident" that this repo tries not to accumulate.

### 4.4 Interrupt behaviour

**Level-triggered** on **VIC0 line 6**, asserted for as long as
`(IRQ_STATUS & IRQ_MASK) != 0`. The driver clears bits by writing ones to
`IRQ_STATUS`; when the result is zero the line drops. Level, not edge, because
`core/src/soc/vic.c` already models levels and because an edge-triggered device
that raises a second interrupt while the handler is running loses it.

A useful first cut skips the interrupt entirely — see §4.6.

### 4.5 Device-tree node

Added under `/arm-io`, alongside `sdio`:

```
/device-tree/arm-io/vmnet0
    name              = "vmnet0"
    device_type       = "vmnet"
    compatible        = "vmnet,ios3vm"
    reg               = { 0x07000000, 0x00001000 }
    interrupts        = { 6 }
    interrupt-parent  = <the VIC's phandle: copy the 4 raw bytes from any
                         existing /arm-io child, e.g. uart0's c0 4c b0 00>
    local-mac-address = <6 bytes>
    AAPL,phandle      = <a value not already used in the tree>
```

`AppleARMPlatform` turns every `/arm-io` child into an `AppleARMIODevice` nub
whose `IONameMatch`-able name is its `compatible` string — **CONFIRMED** by
`AppleS5L8900XSDIO` matching `"sdio,s5l8900x"` and `AppleSynopsysOTG2` matching
`"usb-otg,s5l8900x"` in exactly this way.

**Tooling gap, stated:** `tools/bootkernel.c` can only overwrite properties in
place; it cannot add a node. It needs a `dt_add_child()` that splices bytes and
increments the parent's child count. That is a contained change (the format is
`{nprop, nchild}` headers with 32-byte property names and length-prefixed
values), but it is real work and it is on the critical path.

A same-day shortcut exists if that is inconvenient: repurpose an unused existing
node — `jpeg`, `camin` and `tv-out` are all devices we will never model — by
overwriting `name` and `compatible` in place. It works, but it leaves trailing
bytes after the NUL inside the old, longer property, and `compatible` is a
NUL-separated *list*, so the leftovers become a phantom second entry. Acceptable
for one milestone; not acceptable in the tree we ship.

### 4.6 MAC address

A locally-administered unicast address: first octet `0x02`. Derive the low three
octets from the guest's ECID (or generate once) and **persist it next to the NAND
image**, because a MAC that changes on every launch means a new DHCP lease, a new
ARP entry on the host, and a different `en0` identity in the guest's
SystemConfiguration preferences every boot.

### 4.7 The de-risked first cut

For milestone N2 only, the driver can match `IOProviderClass = IOResources` /
`IOResourceMatch = IOKit` (the way `IOUserEthernet` and `IONetworkStack` do —
**CONFIRMED** from their personalities), hardcode the 0x3F000000 window via
`IOMemoryDescriptor::withPhysicalAddress()`, and **poll the RX ring from an
`IOTimerEventSource` at 1 kHz instead of taking an interrupt.**

That removes the device-tree change *and* the interrupt wiring from the riskiest
milestone, leaving exactly one thing under test: does a kext we built load and
run at all. Both are added back in N3.

---

## 5. The host side: getting packets out of an iOS 15 app

### 5.1 The constraint, and why it turns out not to bite

A sandboxed iOS app cannot open raw sockets or create a TUN device. The device is
jailbroken, so more is *possible* — but the recommendation is to **not use any of
it**, and that is a considered choice rather than a concession.

| Approach | Privilege needed | Verdict |
|---|---|---|
| **User-space TCP/UDP NAT** | **none** | **Recommended.** Works as `mobile`, works sandboxed, works on a device that is not jailbroken at all. |
| `utun` on the host | root (INFERRED; on iOS, `com.apple.net.utun_control` is not reachable by an unprivileged app) | Rejected. Also needs `net.inet.ip.forwarding` and `pfctl` rules, i.e. it reconfigures the *whole phone's* networking to give one app a network. |
| `SOCK_RAW` / `/dev/bpf` L2 bridging | root | Rejected. Additionally, bridging a second MAC onto the host's Wi-Fi radio is refused by most consumer APs, and Apple's Wi-Fi driver will not usefully go promiscuous. |

The conclusion is worth stating loudly because it is counter-intuitive:
**guest networking is not blocked on any entitlement, privilege, or jailbreak
question.** It is ordinary application code.

### 5.2 The NAT

The emulator terminates guest Ethernet frames and acts as the gateway. It is the
same idea as QEMU's SLIRP — but **we write our own**: QEMU is GPL, and even if
`libslirp`'s own licence permits reuse (GUESS — it is described as BSD-style, and
that must be checked properly before anyone links it), a dependency contradicts
the zero-dependency property that makes `core/` test in under a second.

Estimated 2–3k lines of C11 in `core/src/net/`, with the same discipline as the
rest of the core: bounds-checked in 64-bit, malformed input rejected with a
status rather than absorbed.

**Virtual topology**

```
  guest 10.0.2.15/24      gateway/NAT 10.0.2.2      DNS 10.0.2.3
```

**What it implements**

- **ARP** for 10.0.2.2 and 10.0.2.3, and gratuitous replies for anything else in
  the subnet so the guest never black-holes.
- **IPv4**, with reassembly refused rather than half-implemented (the guest will
  not fragment at a 1500-byte MTU).
- **ICMP echo** → `socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)`, which Darwin
  permits unprivileged (INFERRED — verified behaviour on macOS, assumed to hold
  on iOS). If it does not, answer echoes locally from the emulator: it makes
  `ping 10.0.2.2` work, which is all milestone N3 needs.
- **UDP** → `SOCK_DGRAM`, keyed on (src, sport, dst, dport) with an idle timer.
- **TCP** → one host `SOCK_STREAM` per guest flow. This is the only genuinely
  hard part: we must run a real, if small, TCP state machine facing the guest —
  sequence and ack tracking, a receive window, retransmission, delayed ACK, MSS
  clamped to 1460, and correct FIN/RST translation in both directions. A
  half-correct TCP produces stalls that look like emulator bugs, which is exactly
  the failure mode this project is worst placed to debug. Hence milestone **N0**:
  it gets written and tested *before* any guest exists.
- **DHCP**, answered by the emulator: hands out 10.0.2.15/24, router 10.0.2.2,
  DNS 10.0.2.3, lease long enough that renewal never matters.
- **DNS**, answered by the emulator: parse the query, call the host's
  `getaddrinfo()`, synthesise the response. This inherits the host's resolver
  configuration — including any VPN or DoH the user has — instead of forwarding
  raw port-53 traffic that a captive network might block.

**What it does not do:** inbound connections (except explicit forwarding rules),
IPv6 (deferred, and iPhone OS 3 barely cares), and anything that is not
TCP/UDP/ICMP.

### 5.3 Threading

The NAT runs on its own thread with a `kqueue` loop. Guest→host frames cross on a
lock-free single-producer/single-consumer queue drained by that thread. Host→guest
frames are pushed into the RX ring under a small mutex and raise the interrupt.
The CPU thread must never block on the network — a full RX ring drops and counts.

---

## 6. What the guest needs beyond a working NIC

### 6.1 Address configuration

Two layers, and both are worth having:

1. **Bootstrap (instant, verifiable).** A launchd job on our root filesystem that
   runs `ifconfig en0 inet 10.0.2.15 netmask 255.255.255.0 up`,
   `route add default 10.0.2.2`, and writes `nameserver 10.0.2.3` to
   `/etc/resolv.conf`. This gets milestones N3–N5 done without touching
   `configd`.
2. **Real (needed for the OS to believe it).** Ship
   `/Library/Preferences/SystemConfiguration/preferences.plist` describing a
   network service on `en0` with `IPv4 { ConfigMethod = DHCP }`. Only then does
   `configd`'s IPConfiguration run DHCP, publish the service into the dynamic
   store, and make `SCNetworkReachability` report reachable — which is what
   CFNetwork, Safari, Mail and every third-party app actually test before they
   will even attempt a connection. INFERRED: the exact plist schema for
   iPhone OS 3.1.3 needs to be read off a real 3.x device backup or the rootfs
   DMG; it is close to but not identical to the macOS 10.6 schema.

### 6.2 What the UI will and will not show

Stated bluntly, because it is the honest cost of not doing Route A:

- **Settings → Wi-Fi: empty.** It is driven by `Apple80211` against
  `IO80211Family`. Our `en0` is a plain Ethernet controller and will never
  appear there.
- **Status bar: no Wi-Fi glyph, no EDGE glyph.** Same reason.
- **`SCNetworkReachability`: reachable, and not flagged WWAN.** So apps that gate
  on reachability — which is nearly all of them — will proceed. Safari, Mail,
  Maps and YouTube will *try*.

Route A buys back the glyph and the Settings pane. Nothing else. That is worth
knowing before anyone spends three months on it.

### 6.3 TLS in 2026, honestly

This is the part that disappoints people, so it is worth being precise.

iPhone OS 3.1.3's SecureTransport does SSLv3 and TLS 1.0, with RSA key exchange
and RC4/3DES/AES-CBC-SHA1 suites; no ECDHE, no AES-GCM, no TLS 1.2, and probably
no SNI (INFERRED from the era; the exact suite list should be read out of the
guest's `Security.framework` before anyone relies on it). Its root store is
frozen in 2010 and therefore lacks ISRG Root X1 and every root issued since.

Consequences:

- **Plain HTTP: works.** This is the target for milestone N5.
- **HTTPS to the modern internet: essentially everything fails**, and it fails
  three independent ways at once — protocol version, cipher suite, and an
  untrusted root. Any one of them is fatal.
- **Apple's own 2010 services are gone.** Activation (`gs.apple.com`), the iTunes
  and App Stores, Maps' tile API, the YouTube app's API, the Weather feed — dead
  endpoints or dead protocols. iPhone OS 3's App Store is not coming back. Say
  this out loud in the README when the time comes rather than letting people
  discover it.

**The fix that actually works** is a TLS-terminating proxy, in two escalating
forms:

1. **Explicit proxy (cheap).** Run an HTTP/HTTPS proxy inside the emulator app;
   point the guest at it through CFNetwork's proxy settings in the same
   SystemConfiguration preferences we already ship. Ten lines of plist, and
   `CONNECT` handling in the host app.
2. **Transparent interception (what makes Safari genuinely usable).** The NAT
   redirects guest port 443 to a local listener that speaks TLS 1.0/RC4 *down* to
   the guest and TLS 1.3 *up* to the internet, presenting certificates signed by
   a CA we generate once and add to the guest's trust store. We build the root
   filesystem, so installing the CA is a file copy. This makes *unmodified* guest
   apps work over HTTPS.

Even with TLS solved, Safari 3.x has no modern JavaScript or CSS. Expect the
demo to be a deliberately period-appropriate or simple page, and expect the
modern web to look broken. That is not a bug in the emulator.

---

## 7. Staged plan

Each stage has something you can look at. The stages before N2 need no phone and
no guest at all.

| | Stage | Observable |
|---|---|---|
| **N0** | **Host NAT, standalone.** `core/src/net/` plus a `tools/nettest` harness that feeds hand-built Ethernet frames in and prints what comes back. | `ctest` goes green on: an ARP reply, an ICMP echo reply, a DHCP offer, a DNS answer, and a full synthetic TCP handshake that fetches `GET /` from a real host. **No guest, no phone, sub-second.** |
| **N1** | **Device model.** `core/src/soc/vmnet.c` + `core/tests/test_vmnet.c`, rings driven directly from C. | A frame written into the TX ring reaches the NAT; a frame injected by the NAT lands in the RX ring with the interrupt asserted and cleared correctly. |
| **N2** | **Guest sees an interface.** ⚠️ *The milestone that proves the approach.* Build the kext, pack it as mkext1, load it into guest RAM, rename `MemoryMapReserved-4` → `Driver-XXXXXXXX`. Use the `IOResources` + polling variant (§4.7) so only one thing is under test. | Our kext's `start()` printf appears in the XNU UART log, and `IONetworkStack` registers `en0` with our MAC. |
| **N3** | **ARP and ICMP.** Add the device-tree node and the real interrupt. Static `ifconfig` from a launchd job. | `ping 10.0.2.2` from the guest replies. Then `ping 8.8.8.8` traverses the host. |
| **N4** | **TCP.** | A small static test binary on our root filesystem completes a handshake to a host-side listener, then to a real internet host, and echoes bytes back. |
| **N5** | **A real HTTP fetch.** | `GET http://example.com/` from the guest prints HTML to the console. **This is "the guest is on the internet."** |
| **N6** | **The OS believes it.** SystemConfiguration preferences, DHCP through `configd`, reachability. | **MobileSafari renders a live web page.** A 2009 phone's browser, running on a 2015 phone, loading today's internet. This is the shareable artefact. |
| **N7** | **TLS.** Proxy plus a guest CA. | Safari over HTTPS. |
| **N8** | *(optional trophy)* Route A. | Settings → Wi-Fi lists a network, you tap it, and the status-bar glyph appears. |

**N0, N1 and the NAT are not blocked on M5.** They can be built and finished
today, against the current milestone state. **N2 needs the kernel to reach IOKit
matching**, which it does. **N6 needs SpringBoard**, so the *demo* waits for M5
even though the *networking* does not.

---

## 8. Risks, in order of how likely they are to hurt

1. **The `armv6` iPhoneOS kext toolchain. This is the one to spike first.**
   Apple never shipped a `Kernel.framework` in the iPhoneOS SDK and never
   released the ARM XNU sources. We must produce an `MH_KEXT_BUNDLE` for `armv6`
   whose C++ ABI and `OSObject` vtable layout match a kernel built by GCC 4.2 in
   2009, linking against `IONetworkingFamily` 1.8 headers taken from Apple's
   open-source *macOS* release of the same vintage. If clang's ARM vtable layout
   diverges, we get a kext that links cleanly and then branches into space.
   *Mitigations:* our own emulator is the ideal debugger for exactly this — we
   see every fetch. And route C2 removes kernel-mode code from the picture
   entirely. **Spike this before anything else: build a kext that does nothing
   but `IOLog("hello")` and get it to print. Everything else in this document is
   ordinary work; this is the only step that might not be possible.**
2. **`record_startup_extensions_function` may never be called on this ARM build.**
   The symbol is present (CONFIRMED); the *call site* is not proven. If the mkext
   path is inert, fall back to `kext_request()` from a small userland helper
   (`_kext_request` is CONFIRMED present) or, last resort, prelink our kext into
   the kernelcache ourselves — which means implementing the static linking that
   kxld would otherwise do for us.
3. **Our TCP against the guest's TCP.** Subtle bugs here masquerade as emulator
   bugs. N0 exists precisely to stop that.
4. **`IOEthernetControllerCreate` may not exist in 3.1.3's `IOKit.framework`**,
   which would remove the C2 fallback. Cheap to check: dump the guest's dyld
   shared cache from the root filesystem DMG. **Do it early**, because it decides
   whether we have one plan or two.
5. **The SystemConfiguration plist schema for 3.x** is INFERRED. Worst case, N6
   slips and we live on the static-`ifconfig` bootstrap for longer.

---

## 9. Licensing

This project is MIT and ships no Apple code. That constrains what may be
consulted and what may be copied:

- **QEMU, `qemu-ios`, openiBoot, and Linux's `libertas` Marvell driver are GPL.**
  They may be consulted for *facts* — register offsets, protocol sequences,
  documented constants — and never copied. Route A in particular would lean on
  `libertas` for facts about the 88W8686 command set; the implementation must be
  ours.
- **`libslirp` is described as BSD-licensed** (GUESS — verify properly before
  anyone links it). Irrelevant in practice: we write our own NAT anyway, because
  a dependency would break the zero-dependency property of `core/`.
- **Apple's `IONetworkingFamily` headers are APSL-2.0.** They are a *build-time*
  dependency, used the way an SDK is used. They are not vendored into this
  repository and no APSL file is redistributed under MIT.
- **The SDIO Simplified Specification is public**, and the mkext container format
  is documented in xnu's `libkern/libkern/mkext.h`. Our writer
  (`tools/mkmkext.c`) is a clean reimplementation, and can reuse the LZSS
  compressor already in `core/src/firmware/lzss.c`.

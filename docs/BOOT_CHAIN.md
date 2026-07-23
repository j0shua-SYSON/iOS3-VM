# The iPhone OS 3 boot chain: current path and full-chain target

The long-term target is to satisfy each stage of Apple's secure boot chain as
the silicon does. That is not the path implemented today. `runfw` has executed
an extracted real LLB payload, while `bootkernel` enters XNU directly after
synthesizing a subset of iBoot's handoff. SecureROM is not modelled and iBoot
itself has not executed.

```
  SecureROM (bootrom)   ← baked into the SoC; not modelled
        │  loads + verifies
        ▼
  LLB (Low-Level Bootloader)   ← extracted payload runs in runfw
        │
        ▼
  iBoot                        ← not yet executed
        │  sets up, loads + verifies
        ▼
  kernelcache (XNU)            ← currently entered directly by bootkernel
        │  hands off
        ▼
  launchd  →  SpringBoard      ← from the root filesystem DMG in the IPSW
```

The arrows describe the full-chain target. They are not a claim that the current
emulator has reproduced each verification or handoff stage.

## What each stage needs from the emulator

| Stage | Emulator must provide |
|---|---|
| SecureROM | **Future full-chain work:** reset vector, initial memory map, crypto engines and DFU/recovery behavior. No SecureROM stub or dump is executed today. |
| LLB / iBoot | The current core/NOR/UART/timer model is sufficient for the recorded standalone LLB run. Executing iBoot and reproducing its signature-verification policy remain future work. |
| kernelcache | The current direct path provides VICs, timers, ARM1176 WFI wake handling, MMU/TLB maintenance, a device tree and an iBoot-like handoff. Historical mode validates a 512 MiB layout and streams the root filesystem into guest RAM. The current cold path instead exact-gates the 7E18 kernel, device tree, and rootfs, fixes guest DRAM at 128 MiB, and serves a create-only work image through guarded md strategy/raw bridges. The real-firmware-tested raw fix exact-patches `_mdevrw` to `svc #0xe3; svc #0xe4`; `ARM_SVC_REDIRECTED` sends a missing user mapping through exact Thumb `_uiomove64` at `0xc0128d14`, using four 128 KiB SP-and-mode-keyed bounce slots below `topOfKernelData`. A zero-initialized coherent 128 KiB in-memory tail preserves XNU's observed no-EOF-check behavior without growing either disk image. Run07 retained two redirects and two completions with no guest raw error or pending continuation through a clean 2 B cap. NAND-controller/VFL/FTL integration remains a separate hardware-fidelity path. |
| launchd → SpringBoard | Historical checkpoint evidence reaches a clean 2.98 B cap without a guest panic or emulator undefined stop, but direct-RAM free pages dipped to 97. The fresh display-enabled 128 MiB external-md run09 retained `launchd`, fsck, the `/dev/md0` root mount, both `mDNSResponder[14]` Seatbelt lines, and `systemShutdown false`, then finished its 2 B cap with 36.5% USR execution and 50.69 MiB at the free-page low. It recorded one exact stock SpringBoard `posix_spawn` pathname attempt at instruction 635,280,837. That proves a launch request, not syscall success, a running process, or rendering. No CLCD MMIO was recorded; SPI0 saw only 13 early platform writes, and the frame remained one 8x16 white block on black. Completion still needs spawn-outcome/process evidence, a real display-driver handoff, multitouch, and enough IOKit-backing devices for remaining userland. |

The synthesized display handoff is being corrected before that last stage is
tested. CLCD offsets `0x0d8..0x0ec` are per-window auxiliary configuration, not
panel timing; the actual `VIDTCON0..3` timing registers live at
`0x20c..0x218`. The N82 seed now carries the iBoot-compatible 54 MHz display
clock divided by five, inverted-VCLK polarity, and porch/sync state.
`VIDTCON2` derives from the requested geometry, with production using 320x480,
and the initial `0x0d8`, `0x0e0`, and `0x0e8` window words are `0x1000`. A
configured window counts as live scanout only while start state, `CLCD_CTRL`
global enable, and `VIDCON0` bit 0 are all active. This removes false-positive
frames and wake events. Run09 confirmed that the corrected seed survives a
two-billion-instruction real-firmware run and that launchd attempts the exact
stock SpringBoard path, but the guest made no recorded CLCD MMIO access and
produced no meaningful frame. Bundle-range PC observations, a launch-request
entry, and a retained seed are evidence of the frontier, not proof of syscall
success, driver start, SpringBoard execution, or rendering.

The accepted kernel, device tree, and rootfs source files remain original and
immutable. Exact firmware-specific patches and device-tree edits touch only
loaded guest RAM; fstab and volume-growth edits touch only the separate
create-only work image.

## IMG3 — Apple's firmware container

3.x-era firmware images are wrapped in the **IMG3** format: a tagged container
holding the payload, its encryption info and signature material. The current
loader:

1. Parse the IMG3 tags (`TYPE`, `DATA`, `KBAG`, `SHSH`, `CERT`, …).
2. Decrypt the `DATA` payload with **AES-128/256-CBC** using the image's key+IV.
3. Records whether `SHSH` and `CERT` are present, but **does not verify their RSA
   signatures**. Parsing/decryption is therefore not a secure-boot trust result.

## Firmware & keys — you supply your own

**iOS3-VM ships no Apple firmware.** Apple firmware is copyrighted; distributing
it is not something this project does. Instead, at runtime you provide:

1. **An iPhone OS 3.1.3 IPSW** for the matching device (an S5L8900 model). IPSWs
   are still widely archived; you download your own.
2. **The decryption key and IV** for each encrypted image. The repository has no
   `keys.json` loader: pass these values explicitly to the relevant tool or
   provide already-extracted inputs to the CLI harness.

The repository-root `firmware/` directory is ignored by default. That is a
convenience, not a security boundary: inspect staged files before every push and
never force-add firmware, keys or decrypted Apple payloads. The app currently has
no firmware importer. You are responsible for using material you are entitled to
use and for following applicable law.

## Inspecting your IPSW

Two host tools make a real IPSW immediately useful, and deliberately run our
*real* parser against real bytes — so inspection doubles as validation:

```sh
# What is actually inside? (an IPSW is a ZIP)
python tools/ipsw_explore.py firmware/iPhone1,2_3.1.3_7E18_Restore.ipsw

# Pull out one container...
python tools/ipsw_explore.py <ipsw> -x iBoot.n82ap.RELEASE.img3 -o firmware/iboot.img3

# ...and see whether our parser agrees with Apple's actual layout
./build/core/img3dump firmware/iboot.img3

# With a published key, decrypt the payload
./build/core/img3dump firmware/iboot.img3 \
    -k <hexkey> -iv <hexiv> -o firmware/iboot.bin

# Scan a NOR dump for containers
./build/core/img3dump -s firmware/nor.bin
```

`img3dump` prints the raw header bytes before parsing. That matters: a
byte-swapped magic constant once survived a fully green test suite because our
fixtures shared the same mistake as our code. Seeing the real bytes is how that
class of error gets caught.

Before committing, verify that firmware remains untracked, for example with
`git status --short` and `git diff --cached --name-only`. Git ignore rules can
be bypassed and do not prevent data from being added under another path.

## Why 3.1.3 specifically

- It's the **last** iPhone OS 3.x release — the most complete 3.x to target.
- Its device keys are **public**, so decryption is a solved problem.
- It runs on the **S5L8900**, the iPhone 2G/3G application processor we emulate.

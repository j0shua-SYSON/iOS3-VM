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
| kernelcache | The current direct path provides VICs, timers, ARM1176 WFI wake handling, MMU/TLB maintenance, a device tree and an iBoot-like handoff. It validates the full 512 MiB layout and streams the root filesystem directly into final guest RAM; NAND-controller/VFL/FTL integration is a separate hardware-fidelity path, not a requirement of this recorded direct boot. |
| launchd → SpringBoard | Current checkpoint evidence reaches a clean 2.98 B cap without a guest panic or emulator undefined stop; an earlier interval has `launchd` and `mDNSResponder` alive and another observes a new `_execve` with `systemShutdown false`. The former ARMv6 `UXTB16` stop is implemented and replay-cleared. Free pages dipped to 97 and ended at 214 against a 250-page target. It has no SpringBoard frame. Completion still needs the audited host-backed md bulk-copy path for the pinned RAM disk, the LCD/framebuffer path under the real session, multitouch, and enough IOKit-backing devices for the remaining userland. |

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

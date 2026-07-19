# The iPhone OS 3 boot chain (and what iOS3-VM must provide)

To boot a real Apple OS we have to satisfy each stage of Apple's secure boot
chain, exactly as the silicon does. This is the map.

```
  SecureROM (bootrom)   ← baked into the SoC; we provide a small stub / dump
        │  loads + verifies
        ▼
  LLB (Low-Level Bootloader)   ← from IPSW, IMG3-wrapped, encrypted
        │
        ▼
  iBoot                        ← from IPSW, IMG3-wrapped, encrypted
        │  sets up, loads + verifies
        ▼
  kernelcache (XNU)            ← from IPSW, IMG3-wrapped, encrypted
        │  hands off
        ▼
  launchd  →  SpringBoard      ← from the root filesystem DMG in the IPSW
```

## What each stage needs from the emulator

| Stage | Emulator must provide |
|---|---|
| SecureROM | Reset vector, initial memory map, the AES/SHA/PKE crypto engines, DFU/recovery behavior. Can start from a stub that jumps to LLB. |
| LLB / iBoot | A working ARM1176 core, MMU, NOR (via SPI), UART for serial, timers, the clock/PMU, and **IMG3 decryption** (AES-CBC with per-image keys + RSA signature check). |
| kernelcache | Interrupt controller (VIC), timers, cache/TLB maintenance ops, the full device tree the kernel walks, and NAND (with FTL/VFL) presenting the partitions. |
| launchd → SpringBoard | The LCD/framebuffer, multitouch controller, and enough IOKit-backing devices for the userland to come up. |

## IMG3 — Apple's firmware container

3.x-era firmware images are wrapped in the **IMG3** format: a tagged container
holding the payload, its encryption info, and an RSA signature. Our loader
(M3) must:

1. Parse the IMG3 tags (`TYPE`, `DATA`, `KBAG`, `SHSH`, `CERT`, …).
2. Decrypt the `DATA` payload with **AES-128/256-CBC** using the image's key+IV.
3. Optionally verify the RSA signature (an emulator can relax this).

## Firmware & keys — you supply your own

**iOS3-VM ships no Apple firmware.** Apple firmware is copyrighted; distributing
it is not something this project does. Instead, at runtime you provide:

1. **An iPhone OS 3.1.3 IPSW** for the matching device (an S5L8900 model). IPSWs
   are still widely archived; you download your own.
2. **The decryption keys**, which for 3.x-era devices are **published** on the
   community firmware-key wiki. The emulator reads a small `keys.json` you drop
   in next to the IPSW.

The app's firmware directory is git-ignored precisely so no copyrighted image is
ever committed. Think of it like a console emulator: the emulator is legal and
open; the ROM is yours to provide.

## Why 3.1.3 specifically

- It's the **last** iPhone OS 3.x release — the most complete 3.x to target.
- Its device keys are **public**, so decryption is a solved problem.
- It runs on the **S5L8900**, the iPhone 2G/3G application processor we emulate.

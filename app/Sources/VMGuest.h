/*
 * iOS3-VM — the on-device demo guest.
 *
 * This is the payload the app runs so that the framebuffer path can be shown to
 * WORK before real Apple firmware is available on the phone. It is deliberately
 * *not* a mock: the pattern on screen is produced by real ARM instructions
 * decoded by core/src/arm/arm_interp.c, whose STR goes through the S5L8900 bus
 * in core/src/soc/machine.c and lands in the same guest DRAM the XNU kernel
 * paints its console into. The app only ever reads that DRAM.
 *
 * The geometry and pixel format match what tools/bootkernel.c advertises to the
 * kernel in Boot_Video — 320x480, 32 bits per pixel, bytes stored B,G,R,A — so
 * the display path exercised here is the display path the kernel will use.
 *
 * Written in plain C11 rather than Objective-C on purpose: it can then be
 * compiled and executed against the real core on a non-Apple host, which is how
 * the instruction encodings below were verified.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_APP_VMGUEST_H
#define IOS3VM_APP_VMGUEST_H

#include "soc.h"

/* The original iPhone panel, as advertised in Boot_Video. */
#define VM_FB_WIDTH   320u
#define VM_FB_HEIGHT  480u
#define VM_FB_BPP     4u
#define VM_FB_BYTES   (VM_FB_WIDTH * VM_FB_HEIGHT * VM_FB_BPP)   /* 614400 */

/* The machine the app builds: DRAM where the S5L8900 puts it, 128 MB as on the
 * hardware. See VMEngine.m for why allocating this much is affordable. */
#define VM_GUEST_RAM_BASE  S5L8900_SDRAM_BASE
#define VM_GUEST_RAM_SIZE  (128u * 1024u * 1024u)

/* The payload blob (code + literal pool + strings) loaded at the bottom of
 * DRAM. Fixed size so the literal pool can sit at a known offset. */
#define VM_GUEST_BLOB_BYTES 0x400u

/*
 * Where the framebuffer lives: the top of DRAM, page aligned. This mirrors
 * tools/bootkernel.c, which puts it there because the kernel's own page tables
 * grow up from topOfKernelData and must not collide with it.
 * Returns 0 if the machine's DRAM is too small to hold one.
 */
uint32_t vm_guest_fb_pa(uint32_t ram_base, uint32_t ram_size);

/*
 * Load the demo payload into `m` and point the CPU at it. Returns false if the
 * machine has no DRAM or too little of it. After this, s5l8900_run() executes
 * the guest; it never terminates by design.
 */
bool vm_guest_install(s5l8900_t *m);

/*
 * Host pointer to the framebuffer this demo paints, inside `m`'s DRAM, or NULL.
 * Valid until s5l8900_free(). Reading it is a read of guest memory, nothing
 * more. This is the fixed address the payload targets; prefer vm_guest_display()
 * below, which asks the display controller where the framebuffer actually is.
 */
const uint8_t *vm_guest_framebuffer(const s5l8900_t *m);

/* 32-bit component order in memory: BGRA is byte0=B..byte3=A (what this SoC's
 * framebuffer uses and what XNU's console writes); ARGB is the reverse. */
typedef enum { VM_ORDER_BGRA = 0, VM_ORDER_ARGB = 1 } vm_pixel_order_t;

/*
 * Locate the framebuffer the emulated display controller is scanning out, and
 * report its geometry and byte order. This is the honest path: when the core
 * provides a CLCD model, it reads back window 0 exactly as the guest (or the
 * iBoot stand-in in vm_guest_install) programmed it — address, size and pixel
 * order all come from the hardware model, not from a constant here. When the
 * core has no CLCD, it falls back to the fixed framebuffer this demo paints.
 *
 * Returns a host pointer valid until s5l8900_free(), or NULL. Any of the out
 * parameters may be NULL. The returned buffer is guaranteed to lie within DRAM
 * and to be no larger than VM_FB_BYTES.
 */
const uint8_t *vm_guest_display(const s5l8900_t *m,
                                uint32_t *width, uint32_t *height,
                                uint32_t *stride, vm_pixel_order_t *order);

#endif /* IOS3VM_APP_VMGUEST_H */

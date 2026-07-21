/*
 * iOS3-VM — Samsung S5L8900 system-on-chip model.
 *
 * The S5L8900 is the application processor in the original iPhone, the iPhone
 * 3G, and the iPod touch 1G — the silicon iPhone OS 1–3 ran on. This header
 * declares its memory map and the device models the CPU talks to over the bus.
 *
 * The peripheral base addresses below come from public reverse-engineering of
 * the S5L8900. They are exercised by our own bare-metal payloads today and will
 * be validated against real Apple firmware at M3.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_SOC_H
#define IOS3VM_SOC_H

#include <stddef.h>
#include <stdint.h>
#include "arm.h"

/* ------------------------------------------------------------ memory map */
#define S5L8900_SRAM_BASE   0x22000000u
#define S5L8900_SDRAM_BASE  0x08000000u
#define S5L8900_UART0_BASE  0x3cc00000u
#define S5L8900_VIC0_BASE   0x38e00000u
#define S5L8900_TIMER_BASE  0x3e200000u
#define S5L8900_CLOCK_BASE  0x3c500000u
/*
 * GPIO. CONFIRMED against two independent sources: the shipped device tree's
 * /arm-io/gpio has reg {0x6400000,0x1000} and arm-io maps child+0x38000000, and
 * a walk of the guest's live page tables from the VA AppleS5L8900XGPIOIC prints
 * lands on the same page. The value here was previously 0x3cf00000 — the
 * S5L8720-era GPIO address, wrong for this SoC. It was unused, so it was a
 * landmine rather than a live bug; it is corrected rather than left to be
 * "confirmed" by whoever wired it up next.
 */
#define S5L8900_GPIO_BASE   0x3e400000u
#define S5L8900_MIU_BASE    0x38100000u   /* clkrstgen's second reg range */
#define S5L8900_EDGEIC_BASE 0x38e02000u
#define S5L8900_DEV_SIZE    0x00001000u   /* per-peripheral window */

/*
 * There are TWO PL192 VICs, not one. The device tree gives the block as
 * reg {0xe00000, 0x2000} with vic-stride 0x1000, and AppleARMPL192VIC maps both
 * pages. The interrupt numbering is flat across the pair: /arm-io/gpio lists
 * lines 0x20 and 0x21, /arm-io/wdt line 0x33, /arm-io/sdio line 0x2a — all past
 * VIC0's 32 lines, so they can only be VIC1 lines 0, 1, 19 and 10.
 */
#define S5L8900_VIC1_BASE   0x38e01000u
#define S5L8900_VIC_COUNT   2u

/* --------------------------------------------------------------- UART ---
 * Samsung-style UART (as on S3C-family parts). Writing a byte to UTXH
 * transmits it; UTRSTAT reports the transmitter permanently ready, so guest
 * "wait until TX empty" spin loops make progress immediately.
 */
#define UART_ULCON   0x00u
#define UART_UCON    0x04u
#define UART_UFCON   0x08u
#define UART_UMCON   0x0cu
#define UART_UTRSTAT 0x10u
#define UART_UERSTAT 0x14u
#define UART_UFSTAT  0x18u
#define UART_UMSTAT  0x1cu
#define UART_UTXH    0x20u
#define UART_URXH    0x24u
#define UART_UBRDIV  0x28u

#define UART_TX_BUFFER 8192

typedef struct {
    uint32_t ulcon, ucon, ufcon, umcon, ubrdiv;
    char     tx[UART_TX_BUFFER];   /* everything the guest has printed */
    size_t   tx_len;
} s5l_uart_t;

void     s5l_uart_reset(s5l_uart_t *u);
uint32_t s5l_uart_read(s5l_uart_t *u, uint32_t off);
void     s5l_uart_write(s5l_uart_t *u, uint32_t off, uint32_t val);

/* ---------------------------------------------------------------- VIC ---
 * PL190-style vectored interrupt controller. Devices assert lines into `raw`;
 * the controller ORs in software interrupts, masks by `enable`, and routes each
 * line to IRQ or FIQ according to `select`.
 */
#define VIC_IRQSTATUS    0x00u
#define VIC_FIQSTATUS    0x04u
#define VIC_RAWINTR      0x08u
#define VIC_INTSELECT    0x0cu
#define VIC_INTENABLE    0x10u
#define VIC_INTENCLEAR   0x14u
#define VIC_SOFTINT      0x18u
#define VIC_SOFTINTCLEAR 0x1cu
#define VIC_VECTADDR0    0x100u  /* per-source ISR address bank, 32 entries    */
#define VIC_VECTADDR     0xf00u  /* PL192 vectored dispatch: read = source|bit31, write = EOI */

typedef struct { uint32_t raw, enable, select, soft; } s5l_vic_t;

void     s5l_vic_reset(s5l_vic_t *v);
uint32_t s5l_vic_read(s5l_vic_t *v, uint32_t off);
void     s5l_vic_write(s5l_vic_t *v, uint32_t off, uint32_t val);
void     s5l_vic_set_line(s5l_vic_t *v, unsigned line, bool level);
bool     s5l_vic_irq(const s5l_vic_t *v);
bool     s5l_vic_fiq(const s5l_vic_t *v);
/* PL192 VICADDRESS: highest-priority pending source tagged with bit 31, or 0.
 * base_source positions this VIC in the daisy chain (0 for VIC0, 32 for VIC1). */
uint32_t s5l_vic_vectaddr(const s5l_vic_t *v, unsigned base_source);

/* -------------------------------------------------------------- timer ---
 * The real S5L8900 timer block, as used by XNU rather than as invented by us.
 *
 * The layout below is not guesswork: instrumenting the bus and correlating each
 * access against the kernel's own symbols shows exactly which registers matter
 * and what it expects of them.
 *
 *   _s5l8900x_get_timebase    reads 0x080/0x084 as a free-running 64-bit
 *                             counter. This is mach_absolute_time(). It must
 *                             count whether or not any timer is "enabled", or
 *                             every delay loop in the kernel spins forever.
 *   _pe_arm_init_interrupts   programs timer 4 at 0x0A0-0x0AF and then routes
 *                             VIC line 7 to *FIQ*, not IRQ.
 *   _s5l8900x_set_decrementer writes the next deadline to 0x0A8.
 *   _fleh_fiq_s5l8900x        acknowledges by writing 0x00030000 to the latch.
 *
 * That acknowledge mask is load-bearing. Latching any other bit pattern leaves
 * the line asserted after the handler returns, which produces an interrupt
 * storm rather than a scheduler tick.
 *
 * The status alias at 0x10000 sits outside the usual 4 KB peripheral window,
 * which is why the timer's window is widened (see S5L8900_TIMER_SIZE).
 */
#define TIMER_TICKSHIGH  0x0080u   /* free-running counter, high word */
#define TIMER_TICKSLOW   0x0084u   /* free-running counter, low word  */
#define TIMER_CONFIG     0x0088u
#define TIMER4_CONFIG    0x00a0u
#define TIMER4_STATE     0x00a4u   /* bit0 start, bit1 update-from-buffer */
#define TIMER4_COUNTBUF  0x00a8u   /* deadline written by set_decrementer  */
#define TIMER4_COUNTBUF2 0x00acu
#define TIMER4_VALUE     0x00b4u   /* live down-count */
#define TIMER_IRQACK     0x00f4u   /* write-1-to-clear */
#define TIMER_IRQLATCH   0x00f8u
#define TIMER_IRQSTATUS  0x10000u  /* alias, outside the 4 KB window */

#define TIMER4_STATE_START  (1u << 0)
#define TIMER4_STATE_UPDATE (1u << 1)
#define TIMER4_IRQ_BITS  0x00030000u  /* what _fleh_fiq_s5l8900x acks */

/* Widened so TIMER_IRQSTATUS at 0x10000 falls inside the timer's window. */
#define S5L8900_TIMER_SIZE 0x00011000u

#define S5L8900_IRQ_TIMER 7u     /* VIC line the timer drives (routed to FIQ) */

/*
 * The guest's clocks, as we advertise them to it in the device tree.
 *
 * This ratio is a real design parameter, not bookkeeping. On the hardware the
 * CPU runs at 412 MHz while the timebase counts at 6 MHz, so one timebase tick
 * costs about 68 CPU cycles. Advancing the timebase once per retired
 * instruction instead makes guest time run ~68x fast relative to guest work,
 * and the kernel then cannot finish servicing one timer deadline before the
 * next one is already in the past: it clamps the decrementer to its minimum,
 * re-enters immediately, and livelocks. That is not a hypothetical -- it burned
 * 66% of a 200M-instruction boot in the FIQ handler.
 *
 * One retired instruction is treated as one CPU cycle, which is optimistic for
 * an ARM1176 but keeps the ratio honest in the direction that matters.
 */
#define S5L8900_CPU_HZ 412000000u
#define S5L8900_TB_HZ    6000000u

typedef struct {
    uint64_t ticks;              /* free-running; never gated by any enable */
    uint32_t config;
    uint32_t t4_config, t4_state;
    uint32_t t4_count, t4_count2, t4_value;
    uint32_t irqlatch;
} s5l_timer_t;

void s5l_timer_reset(s5l_timer_t *t);
uint32_t s5l_timer_read(s5l_timer_t *t, uint32_t off);
void s5l_timer_write(s5l_timer_t *t, uint32_t off, uint32_t val);
/* Advance by `ticks`; returns true while an interrupt is pending. */
bool s5l_timer_tick(s5l_timer_t *t, uint32_t ticks);

/* ----------------------------------------------------------- NOR flash ---
 * On the S5L8900 the low-level boot images (LLB, iBoot, the device tree, the
 * boot logo) live in a small NOR flash reached over SPI. We model it as a
 * read-only memory-mapped region plus a directory built by *scanning* for IMG3
 * containers.
 *
 * Scanning is deliberate: Apple's exact NOR image-table layout for this SoC
 * could not be verified from a primary source, and guessing a structure would
 * be a silent source of wrong behaviour. Scanning for the IMG3 magic works
 * whatever the surrounding directory format turns out to be, and can be
 * replaced with a real table reader once the layout is confirmed against a
 * genuine dump.
 */
#define S5L8900_NOR_BASE 0x24000000u
#define S5L8900_NOR_SIZE 0x00100000u   /* 1 MiB */
#define S5L_NOR_MAX_IMAGES 16

typedef struct {
    uint32_t ident;     /* IMG3 ident, e.g. 'illb', 'ibot', 'dtre' */
    uint32_t offset;    /* byte offset within the NOR              */
    uint32_t size;      /* container size (fullSize)               */
} s5l_nor_entry_t;

typedef struct {
    uint8_t        *data;
    uint32_t        size;
    s5l_nor_entry_t images[S5L_NOR_MAX_IMAGES];
    unsigned        image_count;
} s5l_nor_t;

/* NOR erase granularity. Programming can only clear bits (as on real flash);
 * an erase restores a whole sector to 0xFF. */
#define S5L8900_NOR_SECTOR 0x1000u

bool     s5l_nor_init(s5l_nor_t *n, uint32_t size);
void     s5l_nor_free(s5l_nor_t *n);
uint32_t s5l_nor_read(const s5l_nor_t *n, uint32_t off, unsigned bytes);
/* Copy `len` bytes into the NOR at `off` (as a factory flasher would). This is
 * an unconditional overwrite, used to lay down an initial image. */
void     s5l_nor_program(s5l_nor_t *n, uint32_t off, const void *src, size_t len);

/*
 * Flash-accurate programming: bits can only go 1 -> 0. Returns false if the
 * write would need to set a bit back to 1 (erase the sector first) or is out of
 * range. This is the path guest writes take, so a guest payload can persist
 * itself into NOR — which is exactly what an untethered jailbreak such as
 * 24kpwn does on this SoC.
 */
bool     s5l_nor_write(s5l_nor_t *n, uint32_t off, uint32_t val, unsigned bytes);

/* Erase one sector back to all-ones. */
bool     s5l_nor_erase_sector(s5l_nor_t *n, uint32_t off);
/* Rebuild the image directory by scanning for IMG3 containers. */
unsigned s5l_nor_scan(s5l_nor_t *n);
/* Find a scanned image by ident; returns NULL if absent. */
const s5l_nor_entry_t *s5l_nor_find(const s5l_nor_t *n, uint32_t ident);

#define S5L_UNMAPPED_LOG 32
#define S5L_DEVLOG        256

/* --------------------------------------------------------- stub windows ---
 * A named, register-backed MMIO window for a peripheral we have identified but
 * not yet modelled.
 *
 * This is a deliberate, bounded exception to this core's "trap what you don't
 * implement" rule, and it is worth being precise about why. For an MMIO window
 * there is no option that avoids making a claim: returning 0 for an unmapped
 * read is *already* a guess, and a demonstrably dangerous one — a driver
 * polling a status bit that reads 0 forever spun on one such window for
 * 3.9 million reads, about 2% of an entire boot.
 *
 * A stub is therefore honest storage rather than invented behaviour: reads
 * return what was last written, writes are recorded, and every window is named
 * and counted so it appears in the report instead of hiding. What a stub must
 * never do is fabricate a value the guest is waiting for. When a driver needs a
 * bit to change on its own, that is a real device model, not a stub, and it
 * belongs in its own file.
 */
/* ------------------------------------------------------ power controller ---
 * The power-gate block. See core/src/soc/power.c for why this is a real model
 * rather than a stub: the guest never writes STATE, so read-back storage would
 * leave it reading zero forever — which is exactly what wedged the boot.
 *
 * Note the page is SHARED. Only 0x00-0x7F belongs to the power controller;
 * 0x80 and above is the GPIO interrupt controller, a different block with a
 * different driver.
 */
#define S5L8900_POWER_BASE   0x39a00000u
#define S5L8900_POWER_SIZE   0x00000080u   /* the rest of the page is GPIOIC */
#define S5L8900_GPIOIC_BASE  0x39a00080u
#define S5L8900_GPIOIC_SIZE  0x00000f80u

#define POWER_CONFIG0   0x00u
#define POWER_CONFIG1   0x04u
#define POWER_SETSTATE  0x08u
#define POWER_ONCTRL    0x0cu   /* write 1 to ungate: clears the STATE bit */
#define POWER_OFFCTRL   0x10u   /* write 1 to gate:   sets the STATE bit   */
#define POWER_STATE     0x14u   /* bit n set == domain n is gated OFF      */
#define POWER_SRAM      0x20u
#define POWER_CFG24     0x24u
#define POWER_CFG28     0x28u

/* 14 domains; the driver masks with 0x3fff and the device tree lists 14. */
#define S5L_POWER_DOMAIN_MASK    0x00003fffu
/* The device tree's power-gate-defaults. See s5l_power_reset. */
#define S5L_POWER_GATE_DEFAULTS  0x000012fcu

typedef struct {
    uint32_t state, cfg0, cfg1, sram, cfg24, cfg28;
} s5l_power_t;

void     s5l_power_reset(s5l_power_t *p);
uint32_t s5l_power_read(s5l_power_t *p, uint32_t off);
void     s5l_power_write(s5l_power_t *p, uint32_t off, uint32_t val);

/* ---------------------------------------------------- CLCD display controller ---
 * The path to pixels. See core/src/soc/clcd.c for the evidence behind every
 * register below; the short version is that AppleH1CLCD's own code was read out
 * of the shipped kernelcache and each offset here is a line of that code.
 *
 * Base and interrupt line are CONFIRMED from the shipped device tree:
 *   /arm-io/clcd  reg {0x900000, 0x1000}  interrupts {0xd}
 * and /arm-io ranges maps child + 0x38000000.
 */
#define S5L8900_CLCD_BASE 0x38900000u
#define S5L8900_IRQ_CLCD  13u

#define CLCD_ENABLE      0x000u  /* write 1: start scanout (written last)      */
#define CLCD_DISABLE     0x004u  /* write 1: stop scanout                      */
#define CLCD_CTRL        0x008u  /* display + per-window enables               */
#define CLCD_FIFO        0x00cu  /* per-window FIFO thresholds                 */
#define CLCD_INTMASK     0x014u  /* interrupt enable mask                      */
#define CLCD_INTSTATUS   0x018u  /* interrupt status, WRITE-1-TO-CLEAR         */
#define CLCD_REG1C       0x01cu  /* cleared to 0 on start and on stop          */
#define CLCD_PREENABLE   0x020u  /* write 1 just before CLCD_ENABLE            */
#define CLCD_BACKDROP    0x024u  /* backdrop colour, ARGB                      */
#define CLCD_VIDEO_FIRST 0x028u  /* 11 video/YUV overlay regs, 0x028..0x054    */
#define CLCD_VIDEO_LAST  0x054u
#define CLCD_WIN_FIRST   0x058u  /* RGB window k at CLCD_WIN_FIRST + k*0x18    */
#define CLCD_WIN_STRIDE  0x018u
#define CLCD_WIN_COUNT   4u
#define CLCD_UPDATE      0x0d4u  /* write 2 at the head of every window update */
#define CLCD_TIMING0     0x0d8u  /* panel timing: 0x0d8,0x0dc,0x0e0,0x0e4,0x0ec */
#define CLCD_UPDATE2     0x0e8u  /* constant 0x50001000 per update             */
#define CLCD_TIMING4     0x0ecu
#define CLCD_CSC_FIRST   0x1c8u  /* 8 YUV->RGB matrix regs, 0x1c8..0x1e8       */
#define CLCD_CSC_LAST    0x1e8u
#define CLCD_GATE        0x200u  /* clock/scanout gate, bit0                   */
#define CLCD_STATUS      0x204u  /* read-only; see clcd.c, bits[7:6] matter    */
#define CLCD_OPAQUE_FIRST 0x208u /* 0x208..0x218, saved/restored across sleep  */
#define CLCD_OPAQUE_LAST  0x218u
#define CLCD_GAMMA0      0x400u  /* three 256-entry u32 LUTs, 0x400/0x800/0xc00 */
#define CLCD_GAMMA_SIZE  0x400u

/*
 * Window register sub-offsets, from CLCD_WIN_FIRST + k*CLCD_WIN_STRIDE.
 *
 * CONFIRMED, not inferred, since 0xc0705f00 — AppleH1CLCD's "adopt whatever
 * iBoot left running" routine, the vtable slot IOMobileFramebuffer::start calls
 * immediately after start_hardware:
 *
 *     r2 = mapped register base
 *     if      (read32(r2+8) & 0x40) { sl=[r2+0x58] fp=[r2+0x5c] fb=[r2+0x60] r8=[r2+0x64] }
 *     else if (              & 0x20) { sl=[r2+0x70] fp=[r2+0x74] fb=[r2+0x78] r8=[r2+0x7c] }
 *     else if (              & 0x10) { sl=[r2+0x88] fp=[r2+0x8c] fb=[r2+0x90] r8=[r2+0x94] }
 *     else if (              & 0x08) { sl=[r2+0xa0] fp=[r2+0xa4] fb=[r2+0xa8] r8=[r2+0xac] }
 *
 * which pins the four window bases at 0x58/0x70/0x88/0xa0 (stride 0x18), pins
 * the CLCD_CTRL enable bits to 0x40/0x20/0x10/0x08 in that priority order, and
 * pins the field order. The driver then (0xc0706040..0xc0706068) does
 *
 *     width  = (r8 << 5) >> 21;      // geometry bits[26:16]
 *     height = uxth(r8 &~ 0xfc00);   // geometry bits[9:0]
 *     size   = round_up(sl * height, 0x1000);   // sl is bytes per row
 *
 * and wraps `fb` with IOMemoryDescriptor::withPhysicalAddress, so FBADDR is a
 * PHYSICAL address. Nothing here is a guess any more.
 */
#define CLCD_WIN_PITCH     0x00u  /* stride in BYTES                          */
#define CLCD_WIN_CONTROL   0x04u  /* bits[10:8] pixel format, [17:16] order   */
#define CLCD_WIN_FBADDR    0x08u  /* framebuffer PHYSICAL base                */
#define CLCD_WIN_GEOMETRY  0x0cu  /* (width << 16) | height                   */
#define CLCD_WIN_LINEWORDS 0x10u  /* line length in 32-bit words              */
#define CLCD_WIN_POSITION  0x14u  /* (dstX << 16) | dstY                      */

/* CLCD_CTRL window-enable bits, and the order the driver tests them in. */
#define CLCD_CTRL_WIN0   0x40u
#define CLCD_CTRL_WIN1   0x20u
#define CLCD_CTRL_WIN2   0x10u
#define CLCD_CTRL_WIN3   0x08u
#define CLCD_CTRL_VIDEO  0x80u
#define CLCD_CTRL_ENABLE 0x01u    /* start_hardware ORs this in */

/* CLCD_INTSTATUS / CLCD_INTMASK bits. */
#define CLCD_INT_FRAME    0x0001u /* frame (VBL) done — the swap completion   */
#define CLCD_INT_UNDERRUN 0x3f00u /* per-window FIFO underrun; we never set it */

/*
 * Pixel formats, from window control bits[10:8].
 *
 * The DEPTH is confirmed; the fine-grained names are not, and the difference
 * matters. At 0xc0705ff8 the driver switches on exactly this field to pick the
 * IOSurface pixel format it will publish, through a six-entry jump table:
 *
 *     f = (control >> 8) & 7;
 *     switch (f - 2) { case 0..3: fourcc = '565L'; case 4,5: fourcc = 'ARGB'; }
 *     default (f == 0 or 1):      fourcc = '565L';
 *
 * so the driver itself declares 6 and 7 to be 32 bits per pixel and everything
 * else to be 16-bit 5-6-5. It never distinguishes 2 from 3 from 4 from 5, so
 * neither do we: CLCD_FMT_IS_32BPP() is the only classification the binary
 * supports, and s5l_clcd_scanout() decodes on that and nothing finer.
 */
#define CLCD_FMT_SHIFT     8u
#define CLCD_FMT_MASK      0x7u
#define CLCD_FMT_32BPP     7u
#define CLCD_FMT_RGB565    3u
#define CLCD_FMT_RGB555    2u
#define CLCD_FMT_ARGB4444  5u
#define CLCD_FMT_IS_32BPP(f) ((f) == 6u || (f) == 7u)
/* Component order, from window control bits[17:16]; only meaningful at 32bpp. */
#define CLCD_ORDER_SHIFT   16u
#define CLCD_ORDER_MASK    0x3u
#define CLCD_ORDER_BGRA    0u
#define CLCD_ORDER_ARGB    3u

/* Refresh rate we present to the guest, in GUEST time. */
#define S5L_CLCD_REFRESH_HZ 60u

typedef struct {
    uint32_t stride, control, fbaddr, geometry, linewords, position;
} s5l_clcd_window_t;

typedef struct {
    uint32_t enable, disable, ctrl, fifo;
    uint32_t intmask, intstatus, reg1c, preenable, backdrop;
    uint32_t video[(CLCD_VIDEO_LAST - CLCD_VIDEO_FIRST) / 4u + 1u];
    s5l_clcd_window_t win[CLCD_WIN_COUNT];
    uint32_t update, update2;
    uint32_t timing[5];                      /* 0x0d8,0x0dc,0x0e0,0x0e4,0x0ec */
    uint32_t csc[(CLCD_CSC_LAST - CLCD_CSC_FIRST) / 4u + 1u];
    uint32_t gate;
    uint32_t opaque[(CLCD_OPAQUE_LAST - CLCD_OPAQUE_FIRST) / 4u + 1u];
    uint32_t gamma[3][256];

    bool     scanning;        /* CLCD_ENABLE has been written 1, no stop since */
    uint32_t frame_ticks;     /* timebase ticks per frame; 0 disables the VBL  */
    uint32_t frame_accum;
    uint64_t frames;          /* how many VBLs we have raised (visibility)     */
} s5l_clcd_t;

void     s5l_clcd_reset(s5l_clcd_t *c);
uint32_t s5l_clcd_read(s5l_clcd_t *c, uint32_t off);
void     s5l_clcd_write(s5l_clcd_t *c, uint32_t off, uint32_t val);
/* Advance by `ticks` timebase ticks; returns true while the controller's
 * interrupt output is asserted (status AND mask, as the hardware ANDs them). */
bool     s5l_clcd_tick(s5l_clcd_t *c, uint32_t ticks);

/*
 * Pre-seed the controller the way iBoot leaves it: window 0 programmed and
 * enabled over an already-running scanout. IOMobileFramebuffer::start then
 * adopts the framebuffer verbatim instead of having to invent one.
 *
 * `format` is a CLCD_FMT_* value and `order` a CLCD_ORDER_* value; `stride` is
 * in bytes. This is a host-side call, not a guest-visible register: a boot stub
 * standing in for iBoot calls it before the guest runs.
 */
void     s5l_clcd_seed_window0(s5l_clcd_t *c, uint32_t fb_phys,
                               uint32_t width, uint32_t height,
                               uint32_t stride, uint32_t format, uint32_t order);

/* Read back a window's programming. Returns false if `k` is out of range or the
 * window is not enabled in CLCD_CTRL. */
bool     s5l_clcd_window(const s5l_clcd_t *c, unsigned k,
                         uint32_t *fb_phys, uint32_t *width, uint32_t *height,
                         uint32_t *stride, uint32_t *format, uint32_t *order);

/*
 * Which window the DRIVER would scan out, tested in the driver's own priority
 * order (window 0, then 1, then 2, then 3 — 0xc0705f10..0xc0705f94). Returns
 * CLCD_WIN_NONE when CLCD_CTRL enables no window at all, which is not a
 * cosmetic state: with no bit set the driver leaves its four locals holding
 * whatever the caller's frame did and builds an IOSurface out of that garbage,
 * so "no window enabled" is a bug in whatever stood in for iBoot, not a blank
 * screen. Callers must treat CLCD_WIN_NONE as an error.
 */
#define CLCD_WIN_NONE 0xffffffffu
uint32_t s5l_clcd_active_window(const s5l_clcd_t *c);

/*
 * Scan out an enabled window into 24-bit RGB, one byte per channel, top row
 * first — the seam clcd.c's header has always named, now that the window layout
 * is confirmed rather than inferred.
 *
 * `ram`/`ram_base`/`ram_len` describe the guest DRAM the window's PHYSICAL
 * framebuffer address is resolved against; a window pointing outside it is an
 * error, not a black rectangle. `rgb` must hold at least width*height*3 bytes.
 *
 * Returns false — and writes NOTHING — if the window is disabled, the geometry
 * is empty, the stride cannot hold a row, the source does not lie wholly inside
 * guest DRAM, or `rgb` is too small. It never invents a pixel: every returned
 * byte comes from guest memory.
 */
bool     s5l_clcd_scanout(const s5l_clcd_t *c, unsigned k,
                          const uint8_t *ram, uint32_t ram_base, size_t ram_len,
                          uint8_t *rgb, size_t rgb_len,
                          uint32_t *out_w, uint32_t *out_h);

#define S5L_STUB_MAX      16

typedef struct {
    uint32_t    base, size;
    const char *name;
    /* Backing store covers the WHOLE declared window. A fixed-size array was
     * the first design and it was wrong in a way worth remembering: at 64
     * registers it covered offsets 0x000-0x0FC, while the two registers we had
     * actually measured live at 0x320 (GPIO FSEL) and 0x404 (CLOCK0 ADJ2).
     * Both fell past the array and were counted but not stored, so read-back
     * returned 0 — the stub silently failed to be the honest storage it exists
     * to be, for precisely the registers that mattered. Size the backing to
     * the window instead of hoping the window is small. */
    uint32_t   *regs;
    uint32_t    nregs;
    uint64_t    reads, writes;
    uint64_t    oob;                  /* accesses past the backing store */
} s5l_stub_t;

/* ------------------------------------------------------------- machine ---
 * Wires the CPU to RAM and the peripherals through one arm_bus_t.
 */
typedef struct {
    arm_cpu_t  cpu;
    arm_bus_t  bus;
    uint8_t   *ram;
    uint32_t   ram_base;
    uint32_t   ram_size;
    s5l_uart_t  uart0;
    /*
     * Both PL192 VICs. vic[0] is the historical vic0; vic[1] backs the second
     * page at S5L8900_VIC1_BASE, which AppleARMPL192VIC maps and which carries
     * device-tree interrupt lines 32..63.
     */
    s5l_vic_t   vic[S5L8900_VIC_COUNT];
    s5l_timer_t timer;
    s5l_power_t power;
    s5l_clcd_t  clcd;
    s5l_nor_t   nor;
    uint64_t   unmapped_reads;   /* visibility: accesses outside the map */
    uint64_t   unmapped_writes;

    /* Diagnostic: the first distinct addresses the guest touched outside the
     * memory map. When real firmware wanders off, these name the peripheral we
     * have not modelled yet — which is the next thing to build. */
    uint32_t   unmapped_addr[S5L_UNMAPPED_LOG];
    unsigned   unmapped_addr_count;

    /* Diagnostic: log accesses to device windows (not RAM). Real firmware
     * polls hardware to decide what to do next, so seeing exactly which
     * registers it reads is how we learn what it is waiting for. */
    bool       trace_devices;
    uint32_t   dev_addr[S5L_DEVLOG];
    uint32_t   dev_value[S5L_DEVLOG];
    bool       dev_is_write[S5L_DEVLOG];
    unsigned   dev_count;

    /*
     * How fast guest time runs relative to guest work. See S5L8900_CPU_HZ.
     * cpu_hz retired instructions advance the timebase by tb_hz ticks;
     * tb_accum carries the remainder so the ratio stays exact over time.
     */
    uint32_t   cpu_hz, tb_hz;
    uint64_t   tb_accum;

    /* Identified-but-unmodelled peripheral windows. See s5l_stub_t. */
    s5l_stub_t stubs[S5L_STUB_MAX];
    unsigned   stub_count;
    /* Declarations that were refused (table full, overlap, allocation). Zero is
     * the only acceptable value after init; a non-zero count means a window we
     * meant to name is silently unmapped instead. */
    unsigned   stub_declare_failures;
} s5l8900_t;

/*
 * Declare a stub window. `name` must be a string literal or otherwise outlive
 * the machine. Returns false if the table is full or the window overlaps one
 * already declared — silently shadowing a real device would be worse than
 * refusing. Windows larger than S5L_STUB_REGS*4 are accepted; accesses beyond
 * that are counted in `oob` rather than stored, so the shortfall is visible.
 */
bool s5l8900_add_stub(s5l8900_t *m, uint32_t base, uint32_t size,
                      const char *name);

/* Advance the devices and refresh the CPU's interrupt lines. */
void s5l8900_tick(s5l8900_t *m, uint32_t ticks);

/* ram_base/ram_size define where RAM appears. Returns false on allocation
 * failure. Call s5l8900_free() when done. */
bool s5l8900_init(s5l8900_t *m, uint32_t ram_base, uint32_t ram_size);
void s5l8900_free(s5l8900_t *m);

/* Copy a blob into guest RAM at a physical address. */
void s5l8900_load(s5l8900_t *m, uint32_t addr, const void *data, size_t len);

/* Run up to max_steps instructions, stopping early on a non-OK status.
 * Returns the number of instructions retired. */
unsigned s5l8900_run(s5l8900_t *m, unsigned max_steps, arm_status_t *status);

#endif /* IOS3VM_SOC_H */

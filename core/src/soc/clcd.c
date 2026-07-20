/*
 * iOS3-VM — S5L8900 CLCD display controller (AppleH1CLCD's hardware).
 *
 * This is the block between a booting kernel and a screen. Nothing here is
 * guessed from a datasheet — no S5L8900 documentation exists — every offset and
 * every bit below is a line of AppleH1CLCD's own ARM code, read out of the
 * AppleH1DisplayDrivers kext inside the shipped kernelcache (text at
 * 0xc0703000, size 0xa000). The driver keeps its mapped register base in
 * `this+0x1fc`, so any `ldr/str [rX, #off]` after `ldr rX,[this,#0x1fc]` is a
 * register access and the offset is directly readable. Addresses in the notes
 * below are virtual addresses in that kext, so each claim can be re-checked.
 *
 * WHY THIS IS A DEVICE MODEL AND NOT A STUB
 *
 * Because one bit has to move on its own. AppleH1CLCD submits a framebuffer
 * swap and then waits for the controller to tell it the frame went out:
 *
 *   0xc0705d38  swap_submit tail
 *       if (shadowMask & 1) return;            // a swap is already pending
 *       write32(reg + 0x18, 1);                // clear any stale frame status
 *       shadowMask |= 1;
 *       write32(reg + 0x14, shadowMask);       // enable the frame interrupt
 *
 *   0xc0705d7c  the interrupt handler
 *       status  = read32(reg + 0x18);
 *       pending = status & shadowMask;
 *       if (pending & 1) {
 *           write32(reg + 0x18, 1);            // acknowledge, write-1-to-clear
 *           ... complete the swap, then if nothing else is queued:
 *           shadowMask &= ~1;
 *           write32(reg + 0x14, shadowMask);
 *       }
 *       if (pending & 0x3f00) {
 *           IOLog("AppleH1CLCD: Graphics underrun interrupt: %08x", pending);
 *           write32(reg + 0x18, 0x3f00);
 *       }
 *
 * Storage that returns what was last written would return 0 from 0x018 forever
 * and the swap would never complete: the UI wedges, permanently, with no error.
 * So this model raises the frame interrupt on a timer, in GUEST time.
 *
 * Two properties of that handler are load-bearing and are pinned by tests:
 *
 *   1. The interrupt LINE must be gated by the hardware mask at 0x014, not just
 *      by the status at 0x018. The handler masks the status with its own
 *      software shadow; if the line were asserted while the shadow's bit 0 is
 *      clear, `pending` would come out 0, the handler would acknowledge
 *      nothing, and it would re-enter forever. That is an interrupt storm, not
 *      a display. (This is the same failure shape as the timer's ack mask.)
 *
 *   2. We must NEVER set the underrun bits 0x3f00. There is no underrun in an
 *      emulator with no real FIFO, and setting them only produces error spam
 *      from a driver that is working correctly.
 *
 * WHAT THE DRIVER DOES, in the order it does it
 *
 *   start_hardware      0xc07059d8
 *       write32(reg + 0x218, 1);
 *       shadowMask = 0x3f00;                   // underruns only, for now
 *       write32(reg + 0x18, 0);                // 0x0d8..0x0ec are NOT touched
 *       write32(reg + 0x14, 0);
 *       write32(reg + 0x1c, 0);
 *       v = read32(reg + 8);
 *       v = (v & ~0x30000001) | 1;
 *       v = (v & ~0x02000000) | 0x01000000;
 *       v = (v & ~0x00330000) | 0x00110000;
 *       write32(reg + 8, v);
 *       write32(reg + 0xc, read32(reg + 0xc) | 0x000f0f0f);
 *
 *   enable path         0xc0705840
 *       write32(reg + 0x20, 1);
 *       write32(reg + 0x14, shadowMask);
 *       ... restore panel timing, then program the windows ...
 *       write32(reg + 0x00, 1);                // scanout starts HERE, last
 *       write32(reg + 0x200, read32(reg + 0x200) | 1);
 *
 *   stop  0xc0705694 / 0xc0705910
 *       shadowMask = 0; write32(reg+0x18, 0); write32(reg+0x14, 0);
 *       write32(reg+0x1c, 0); write32(reg+4, 1);
 *       write32(reg + 0x200, read32(reg + 0x200) & ~1);
 *
 *   window update       0xc0704940
 *       write32(reg + 0xd4, 2);
 *       write32(reg + 0xe8, 0x50001000);
 *       ... then the six registers of each enabled window ...
 *
 * REGISTER SEMANTICS, LABELLED
 *
 *   CONFIRMED (read directly out of the driver's code, cited above):
 *     0x000 start, 0x004 stop, 0x008 display+window enables and the exact
 *     read-modify-write masks, 0x00c FIFO thresholds OR 0x000f0f0f,
 *     0x014 interrupt enable, 0x018 interrupt status write-1-to-clear with
 *     bit 0 = frame and 0x3f00 = underrun, 0x01c cleared on start and stop,
 *     0x020 written 1 just before 0x000, 0x0d4 = 2 per update,
 *     0x0e8 = 0x50001000 per update, 0x200 bit 0 gate,
 *     0x204 read-only status used as `if (((v >> 6) & 3) == 3) defer the swap`
 *     (0xc0705ccc..0xc0705ce0), and the register at 0x218 written 1 at start.
 *     Base 0x38900000 and interrupt line 13 are confirmed from the device tree.
 *
 *   CONFIRMED-BY-ABSENCE: 0x0d8, 0x0dc, 0x0e0, 0x0e4, 0x0ec are only ever
 *     saved (0xc070405c..0xc070409c) and restored (0xc07041fc..0xc070423c) by
 *     the sleep path. The driver never initialises them, so iBoot must, and
 *     plain read/write storage is exactly right for us.
 *
 *   INFERRED: 0x024 backdrop colour, the 11 video-overlay registers at
 *     0x028..0x054, the 8 colour-matrix registers at 0x1c8..0x1e8, the opaque
 *     registers at 0x208..0x214 and the three gamma LUTs at 0x400/0x800/0xc00
 *     are all write-then-save/restore as far as the driver is concerned. Their
 *     MEANING is inferred from the surrounding code; their BEHAVIOUR (plain
 *     storage) is what the driver actually requires, and that much is certain.
 *
 *   INFERRED: the layout of the RGB window registers at 0x058 + k*0x18. The
 *     driver writes all six from a helper we did not fully decompile; the
 *     field assignment (pitch, control, framebuffer address, geometry, line
 *     length, position) and the pixel-format encoding in control bits[10:8]
 *     with component order in bits[17:16] come from the disassembly summary
 *     this work was handed. Nothing in the emulator depends on it being right:
 *     the registers are plain storage either way, and s5l_clcd_window() exists
 *     so a wrong reading shows up as a wrong picture rather than as silence.
 *
 *   DECIDED, and stated: 0x204 reads as 0. Bits [7:6] are the only bits the
 *     driver looks at and it defers the swap when they are both set, so any
 *     value with (v >> 6) & 3 == 3 stalls the display. Zero means "not busy",
 *     which is the only answer that cannot deadlock, and it is what we return
 *     unconditionally. We do NOT model whatever real state those bits carry.
 *
 *   NOT MODELLED, deliberately: scanout does not read the framebuffer, gamma is
 *     not applied, the video overlay does nothing, and 0x004 stops the frame
 *     interrupt but nothing else. None of that is invented behaviour — it is
 *     absent behaviour, and s5l_clcd_window() is the seam where a renderer
 *     picks up the real programming when there is one to draw with.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <string.h>

void s5l_clcd_reset(s5l_clcd_t *c) {
    memset(c, 0, sizeof *c);
    /*
     * One frame every S5L_CLCD_REFRESH_HZ of GUEST time. The unit here is
     * timebase ticks, which is what s5l8900_tick feeds us after converting
     * retired instructions at the guest's own CPU:timebase ratio — so the panel
     * refreshes at 60 Hz on the guest's clock, not at 60 Hz of host wall time
     * and not once per N instructions. Getting this wrong in the fast direction
     * is what livelocked the decrementer (see S5L8900_CPU_HZ), and getting it
     * wrong in the slow direction just makes the UI crawl.
     *
     * The machine overrides this from its own tb_hz at init; the default here
     * keeps a standalone s5l_clcd_t honest.
     */
    c->frame_ticks = S5L8900_TB_HZ / S5L_CLCD_REFRESH_HZ;
}

/* Is `off` inside [first, last] on a 4-byte grid? */
static inline bool in_range(uint32_t off, uint32_t first, uint32_t last) {
    return off >= first && off <= last && (off & 3u) == 0u;
}

static s5l_clcd_window_t *window_at(s5l_clcd_t *c, uint32_t off, uint32_t *sub) {
    if (off < CLCD_WIN_FIRST) return NULL;
    uint32_t rel = off - CLCD_WIN_FIRST;
    uint32_t k   = rel / CLCD_WIN_STRIDE;
    if (k >= CLCD_WIN_COUNT) return NULL;
    *sub = rel % CLCD_WIN_STRIDE;
    return &c->win[k];
}

/* The three gamma LUTs are contiguous: 0x400-0x7fc, 0x800-0xbfc, 0xc00-0xffc. */
static uint32_t *gamma_at(s5l_clcd_t *c, uint32_t off) {
    if (off < CLCD_GAMMA0 || off >= CLCD_GAMMA0 + 3u * CLCD_GAMMA_SIZE) return NULL;
    if (off & 3u) return NULL;
    uint32_t rel = off - CLCD_GAMMA0;
    return &c->gamma[rel / CLCD_GAMMA_SIZE][(rel % CLCD_GAMMA_SIZE) / 4u];
}

uint32_t s5l_clcd_read(s5l_clcd_t *c, uint32_t off) {
    switch (off) {
        case CLCD_ENABLE:    return c->enable;
        case CLCD_DISABLE:   return c->disable;
        case CLCD_CTRL:      return c->ctrl;
        case CLCD_FIFO:      return c->fifo;
        case CLCD_INTMASK:   return c->intmask;
        /* Raw status. The driver masks it with its own shadow, so this must be
         * the unmasked latch — handing back status & mask here would hide a
         * frame the driver had already armed. */
        case CLCD_INTSTATUS: return c->intstatus;
        case CLCD_REG1C:     return c->reg1c;
        case CLCD_PREENABLE: return c->preenable;
        case CLCD_BACKDROP:  return c->backdrop;
        case CLCD_UPDATE:    return c->update;
        case CLCD_UPDATE2:   return c->update2;
        case CLCD_GATE:      return c->gate;

        /*
         * MUST NOT read as 0xC0 (or anything with both bits [7:6] set):
         *   0xc0705ccc  ldr r3,[r3,#0x204]; lsr r3,r3,#6; and r3,r3,#3
         *   0xc0705cdc  cmp r3,#3; beq <defer the swap>
         * so a value of 0xC0 makes AppleH1CLCD postpone every swap it is ever
         * asked to do. We return 0 — "not busy" — and model nothing else here,
         * because we do not know what else those bits mean.
         */
        case CLCD_STATUS:    return 0;
        default: break;
    }
    if (in_range(off, CLCD_VIDEO_FIRST, CLCD_VIDEO_LAST))
        return c->video[(off - CLCD_VIDEO_FIRST) / 4u];
    if (in_range(off, CLCD_CSC_FIRST, CLCD_CSC_LAST))
        return c->csc[(off - CLCD_CSC_FIRST) / 4u];
    if (in_range(off, CLCD_OPAQUE_FIRST, CLCD_OPAQUE_LAST))
        return c->opaque[(off - CLCD_OPAQUE_FIRST) / 4u];
    if (off == CLCD_TIMING0 || off == CLCD_TIMING0 + 4u ||
        off == CLCD_TIMING0 + 8u || off == CLCD_TIMING0 + 12u)
        return c->timing[(off - CLCD_TIMING0) / 4u];
    if (off == CLCD_TIMING4) return c->timing[4];
    {
        uint32_t sub;
        s5l_clcd_window_t *w = window_at(c, off, &sub);
        if (w) switch (sub) {
            case CLCD_WIN_PITCH:     return w->stride;
            case CLCD_WIN_CONTROL:   return w->control;
            case CLCD_WIN_FBADDR:    return w->fbaddr;
            case CLCD_WIN_GEOMETRY:  return w->geometry;
            case CLCD_WIN_LINEWORDS: return w->linewords;
            case CLCD_WIN_POSITION:  return w->position;
            default: return 0;
        }
    }
    {
        uint32_t *g = gamma_at(c, off);
        if (g) return *g;
    }
    return 0;
}

void s5l_clcd_write(s5l_clcd_t *c, uint32_t off, uint32_t val) {
    switch (off) {
        /*
         * Start. The driver writes this LAST, after the windows are programmed,
         * so this is the edge on which scanout — and therefore the frame
         * interrupt — begins. Restart the frame accumulator so the first VBL
         * after a start is a whole frame away rather than immediate.
         */
        case CLCD_ENABLE:
            c->enable = val;
            if (val & 1u) { c->scanning = true; c->frame_accum = 0; }
            return;

        /* Stop. Scanout ends; the latched status is left alone because this is
         * not a write-1-to-clear register and the driver clears status
         * separately at 0x018. */
        case CLCD_DISABLE:
            c->disable = val;
            if (val & 1u) c->scanning = false;
            return;

        case CLCD_CTRL:      c->ctrl      = val; return;
        case CLCD_FIFO:      c->fifo      = val; return;
        case CLCD_INTMASK:   c->intmask   = val; return;

        /* Write-1-to-clear. Anything we fail to clear here leaves line 13
         * asserted after the handler returns, which is an interrupt storm. */
        case CLCD_INTSTATUS: c->intstatus &= ~val; return;

        case CLCD_REG1C:     c->reg1c     = val; return;
        case CLCD_PREENABLE: c->preenable = val; return;
        case CLCD_BACKDROP:  c->backdrop  = val; return;
        case CLCD_UPDATE:    c->update    = val; return;
        case CLCD_UPDATE2:   c->update2   = val; return;
        case CLCD_GATE:      c->gate      = val; return;
        /* Read-only: the driver never writes it, and inventing a way for it to
         * move would be inventing behaviour. */
        case CLCD_STATUS:    return;
        default: break;
    }
    if (in_range(off, CLCD_VIDEO_FIRST, CLCD_VIDEO_LAST)) {
        c->video[(off - CLCD_VIDEO_FIRST) / 4u] = val; return;
    }
    if (in_range(off, CLCD_CSC_FIRST, CLCD_CSC_LAST)) {
        c->csc[(off - CLCD_CSC_FIRST) / 4u] = val; return;
    }
    if (in_range(off, CLCD_OPAQUE_FIRST, CLCD_OPAQUE_LAST)) {
        c->opaque[(off - CLCD_OPAQUE_FIRST) / 4u] = val; return;
    }
    if (off == CLCD_TIMING0 || off == CLCD_TIMING0 + 4u ||
        off == CLCD_TIMING0 + 8u || off == CLCD_TIMING0 + 12u) {
        c->timing[(off - CLCD_TIMING0) / 4u] = val; return;
    }
    if (off == CLCD_TIMING4) { c->timing[4] = val; return; }
    {
        uint32_t sub;
        s5l_clcd_window_t *w = window_at(c, off, &sub);
        if (w) { switch (sub) {
            case CLCD_WIN_PITCH:     w->stride    = val; return;
            case CLCD_WIN_CONTROL:   w->control   = val; return;
            case CLCD_WIN_FBADDR:    w->fbaddr    = val; return;
            case CLCD_WIN_GEOMETRY:  w->geometry  = val; return;
            case CLCD_WIN_LINEWORDS: w->linewords = val; return;
            case CLCD_WIN_POSITION:  w->position  = val; return;
            default: return;
        } }
    }
    {
        uint32_t *g = gamma_at(c, off);
        if (g) { *g = val; return; }
    }
}

bool s5l_clcd_tick(s5l_clcd_t *c, uint32_t ticks) {
    if (c->scanning && c->frame_ticks) {
        /*
         * Accumulate rather than test-and-reset against a running counter, so a
         * long tick cannot swallow a frame boundary and a short one cannot
         * double-count it. Frames that fall inside a single large tick are
         * coalesced into one VBL: the status bit is a level, not a count, and
         * the driver only ever completes one swap per interrupt anyway.
         */
        c->frame_accum += ticks;
        if (c->frame_accum >= c->frame_ticks) {
            c->frame_accum %= c->frame_ticks;
            /* Bit 0 only. Never CLCD_INT_UNDERRUN — there is no FIFO here to
             * underrun, and asserting it only makes a correct driver log
             * errors. */
            c->intstatus |= CLCD_INT_FRAME;
            c->frames++;
        }
    }
    /* Gate on the hardware mask, not on the status alone. See the header
     * comment: the handler ANDs the status with its own shadow, so a line
     * asserted for a masked-off source makes it acknowledge nothing and
     * re-enter forever. */
    return (c->intstatus & c->intmask) != 0u;
}

void s5l_clcd_seed_window0(s5l_clcd_t *c, uint32_t fb_phys,
                           uint32_t width, uint32_t height,
                           uint32_t stride, uint32_t format, uint32_t order) {
    s5l_clcd_window_t *w = &c->win[0];
    w->stride    = stride;
    w->control   = ((format & CLCD_FMT_MASK)   << CLCD_FMT_SHIFT) |
                   ((order  & CLCD_ORDER_MASK) << CLCD_ORDER_SHIFT);
    w->fbaddr    = fb_phys;
    /* Width is 11 bits at [26:16], height 10 bits at [9:0]. Mask rather than
     * trust the caller: a 2048-wide request must not spill into the format
     * field and produce a window that reads plausible but is not what was
     * asked for. */
    w->geometry  = ((width & 0x7ffu) << 16) | (height & 0x3ffu);
    w->linewords = stride / 4u;
    w->position  = 0;

    /* Display on, window 0 on. These are the same bits start_hardware forces,
     * so the driver's read-modify-write leaves them set. */
    c->ctrl |= CLCD_CTRL_ENABLE | CLCD_CTRL_WIN0;
    c->gate |= 1u;

    /*
     * iBoot leaves the panel lit and scanning. Starting scanout here means the
     * frame interrupt is already running when the kernel arrives, which is what
     * the hardware would be doing — and it is safe, because the line is gated
     * by the interrupt mask at 0x014 and that is still zero until a driver
     * writes it. Nothing can fire at the guest before it asks for it.
     */
    c->enable   = 1;
    c->scanning = true;
}

bool s5l_clcd_window(const s5l_clcd_t *c, unsigned k,
                     uint32_t *fb_phys, uint32_t *width, uint32_t *height,
                     uint32_t *stride, uint32_t *format, uint32_t *order) {
    static const uint32_t ENABLE_BIT[CLCD_WIN_COUNT] = {
        CLCD_CTRL_WIN0, CLCD_CTRL_WIN1, CLCD_CTRL_WIN2, CLCD_CTRL_WIN3
    };
    if (k >= CLCD_WIN_COUNT) return false;
    if (!(c->ctrl & ENABLE_BIT[k])) return false;

    const s5l_clcd_window_t *w = &c->win[k];
    if (fb_phys) *fb_phys = w->fbaddr;
    if (width)   *width   = (w->geometry >> 16) & 0x7ffu;
    if (height)  *height  =  w->geometry        & 0x3ffu;
    if (stride)  *stride  = w->stride;
    if (format)  *format  = (w->control >> CLCD_FMT_SHIFT)   & CLCD_FMT_MASK;
    if (order)   *order   = (w->control >> CLCD_ORDER_SHIFT) & CLCD_ORDER_MASK;
    return true;
}

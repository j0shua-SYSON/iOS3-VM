/*
 * iOS3-VM — S5L8900 machine: the system bus tying the CPU to RAM and devices.
 *
 * Every guest access lands here after MMU translation, and is routed by
 * physical address to either RAM or a peripheral window. Accesses outside the
 * map are counted rather than silently swallowed, so a misbehaving guest (or a
 * gap in our memory map) is visible instead of mysterious.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------- the window map ---
 *
 * Every window this machine decodes, in one place. It exists so that "is
 * anything shadowed?" is a question with a single answer rather than a property
 * that has to be re-derived from the if-chain in bus_read() every time someone
 * changes the memory map — which is exactly how the NOR came to be unreachable
 * at -R 512 without anyone noticing.
 *
 * NOR's size here is the constant rather than m->nor.size. They are always
 * equal (s5l8900_init passes the constant), and using the constant is what lets
 * s5l8900_ram_conflict() answer before a machine exists.
 */
static const s5l_window_t DEVICE_WINDOWS[] = {
    { S5L8900_NOR_BASE,   S5L8900_NOR_SIZE,   "nor"   },
    { S5L8900_CLCD_BASE,  S5L8900_DEV_SIZE,   "clcd"  },
    { S5L8900_VIC0_BASE,  S5L8900_DEV_SIZE,   "vic0"  },
    { S5L8900_VIC1_BASE,  S5L8900_DEV_SIZE,   "vic1"  },
    { S5L8900_POWER_BASE, S5L8900_POWER_SIZE, "power" },
    { S5L8900_UART0_BASE, S5L8900_DEV_SIZE,   "uart0" },
    { S5L8900_TIMER_BASE, S5L8900_TIMER_SIZE, "timer" },
};
#define NDEVICE_WINDOWS (sizeof DEVICE_WINDOWS / sizeof DEVICE_WINDOWS[0])

/*
 * Physical regions the SoC has, from the shipped device tree. Modelled or not:
 * the point of listing the unmodelled ones is that an oversized DRAM window
 * covering them is a stated property of the configuration, not folklore. See
 * the memory-map block at the top of soc.h for the derivation of each.
 */
static const s5l_window_t SOC_REGIONS[] = {
    { S5L8900_SDRAM_BASE, 0x08000000u,        "dram (128 MB fitted)" },
    { S5L8900_EDRAM_BASE, S5L8900_EDRAM_SIZE, "edram"                },
    { S5L8900_VROM_BASE,  S5L8900_VROM_SIZE,  "vrom"                 },
    { S5L8900_SRAM_BASE,  S5L8900_SRAM_SIZE,  "sram/amc"             },
    { S5L8900_ARMIO_BASE, S5L8900_ARMIO_SIZE, "arm-io"               },
};

bool s5l8900_overlaps(uint32_t a, uint32_t alen, uint32_t b, uint32_t blen) {
    /* 64-bit throughout: a window near the top of the space must not wrap its
     * end below its base and read as disjoint from everything. */
    uint64_t a0 = a, a1 = a0 + alen;
    uint64_t b0 = b, b1 = b0 + blen;
    if (!alen || !blen) return false;
    return b0 < a1 && a0 < b1;
}

unsigned s5l8900_soc_regions(const s5l_window_t **out) {
    if (out) *out = SOC_REGIONS;
    return (unsigned)(sizeof SOC_REGIONS / sizeof SOC_REGIONS[0]);
}

const s5l_window_t *s5l8900_ram_conflict(uint32_t ram_base, uint32_t ram_size) {
    for (unsigned i = 0; i < NDEVICE_WINDOWS; i++)
        if (s5l8900_overlaps(ram_base, ram_size,
                             DEVICE_WINDOWS[i].base, DEVICE_WINDOWS[i].size))
            return &DEVICE_WINDOWS[i];
    return NULL;
}

unsigned s5l8900_windows(const s5l8900_t *m, s5l_window_t *out, unsigned max) {
    unsigned n = 0;
    for (unsigned i = 0; i < NDEVICE_WINDOWS; i++, n++)
        if (out && n < max) out[n] = DEVICE_WINDOWS[i];
    for (unsigned i = 0; i < m->stub_count; i++, n++)
        if (out && n < max) {
            out[n].base = m->stubs[i].base;
            out[n].size = m->stubs[i].size;
            out[n].name = m->stubs[i].name;
        }
    return n;
}

/*
 * Bounds checks are done in 64-bit deliberately. The guest controls every
 * address, so a 32-bit "(a - base) + len <= size" can be made to wrap: an
 * access at 0xFFFFFFFE with len 4 sums to 2, passes the test, and then indexes
 * ram[0xFFFFFFFE]. Widening makes that impossible.
 */
static inline bool in_ram(const s5l8900_t *m, uint32_t a, uint32_t len) {
    if (a < m->ram_base) return false;
    return (uint64_t)(a - m->ram_base) + (uint64_t)len <= (uint64_t)m->ram_size;
}
static inline bool in_window(uint32_t a, unsigned bytes,
                             uint32_t base, uint32_t size) {
    if (!bytes || a < base) return false;
    return (uint64_t)(a - base) + bytes <= (uint64_t)size;
}
static inline bool in_dev(uint32_t a, unsigned bytes, uint32_t base) {
    return in_window(a, bytes, base, S5L8900_DEV_SIZE);
}
static inline bool mmio_word(uint32_t a, unsigned bytes,
                             uint32_t base, uint32_t size) {
    return bytes == 4u && (a & 3u) == 0u && in_window(a, bytes, base, size);
}
/* The timer is the one peripheral that does not fit the uniform 4 KB window:
 * its interrupt-status alias sits at offset 0x10000. */
static inline bool in_power(uint32_t a, unsigned bytes) {
    return mmio_word(a, bytes, S5L8900_POWER_BASE, S5L8900_POWER_SIZE);
}
static inline bool in_timer(uint32_t a, unsigned bytes) {
    return mmio_word(a, bytes, S5L8900_TIMER_BASE, S5L8900_TIMER_SIZE);
}

/* ------------------------------------------------------- stub windows --- */

bool s5l8900_add_stub(s5l8900_t *m, uint32_t base, uint32_t size,
                      const char *name) {
    if (!m || m->stub_count >= S5L_STUB_MAX || !size) return false;
    /* A window that extends beyond the 32-bit physical address space is only
     * partially reachable and used to make the rounded register count wrap. */
    if ((uint64_t)base + size > 0x100000000ull) return false;
    /*
     * Refuse to shadow, or be shadowed by, anything already on the bus: a
     * modelled device, another stub, or RAM. A stub that quietly overlays a
     * real device model would be far harder to notice than a rejected call —
     * and a stub inside the RAM aperture would be unreachable, because RAM is
     * on the fast path (see the routing contract in soc.h).
     */
    for (unsigned i = 0; i < NDEVICE_WINDOWS; i++)
        if (s5l8900_overlaps(base, size,
                             DEVICE_WINDOWS[i].base, DEVICE_WINDOWS[i].size))
            return false;
    for (unsigned i = 0; i < m->stub_count; i++)
        if (s5l8900_overlaps(base, size, m->stubs[i].base, m->stubs[i].size))
            return false;
    if (s5l8900_overlaps(base, size, m->ram_base, m->ram_size)) return false;
    uint64_t nregs64 = ((uint64_t)size + 3u) / 4u;
    if (nregs64 > 0xffffffffu ||
        nregs64 > (uint64_t)SIZE_MAX / sizeof(uint32_t)) return false;
    uint32_t nregs = (uint32_t)nregs64;
    uint32_t *regs = calloc((size_t)nregs, sizeof *regs);
    if (!regs) return false;

    s5l_stub_t *s = &m->stubs[m->stub_count++];
    memset(s, 0, sizeof *s);
    s->base = base; s->size = size; s->name = name;
    s->regs = regs; s->nregs = nregs;
    return true;
}

static s5l_stub_t *find_stub(s5l8900_t *m, uint32_t a, unsigned bytes) {
    for (unsigned i = 0; i < m->stub_count; i++)
        if (in_window(a, bytes, m->stubs[i].base, m->stubs[i].size))
            return &m->stubs[i];
    return NULL;
}

/* Stub windows are honest byte-addressable storage. Assemble accesses a byte
 * at a time so 8/16-bit and unaligned transactions update the correct lanes
 * without ever indexing beyond the rounded backing-register array. */
static uint32_t stub_read(const s5l_stub_t *s, uint32_t addr, unsigned bytes) {
    uint32_t v = 0;
    uint32_t rel = addr - s->base;
    for (unsigned i = 0; i < bytes; i++) {
        uint32_t at = rel + i;
        uint32_t byte = (s->regs[at >> 2] >> ((at & 3u) * 8u)) & 0xffu;
        v |= byte << (i * 8u);
    }
    return v;
}

static void stub_write(s5l_stub_t *s, uint32_t addr,
                       uint32_t val, unsigned bytes) {
    uint32_t rel = addr - s->base;
    for (unsigned i = 0; i < bytes; i++) {
        uint32_t at = rel + i;
        uint32_t shift = (at & 3u) * 8u;
        uint32_t *reg = &s->regs[at >> 2];
        *reg = (*reg & ~(0xffu << shift)) |
               (((val >> (i * 8u)) & 0xffu) << shift);
    }
}

/* ------------------------------------------------------------- reads --- */

/* Record the first distinct out-of-map addresses so a wandering guest tells us
 * which peripheral is missing rather than leaving us to guess. */
static void note_unmapped(s5l8900_t *m, uint32_t addr) {
    uint32_t page = addr & ~0xfffu;
    for (unsigned i = 0; i < m->unmapped_addr_count; i++)
        if (m->unmapped_addr[i] == page) return;
    if (m->unmapped_addr_count < S5L_UNMAPPED_LOG)
        m->unmapped_addr[m->unmapped_addr_count++] = page;
}

static void note_device(s5l8900_t *m, uint32_t addr, uint32_t val, bool is_write) {
    if (!m->trace_devices || m->dev_count >= S5L_DEVLOG) return;
    m->dev_addr[m->dev_count]     = addr;
    m->dev_value[m->dev_count]    = val;
    m->dev_is_write[m->dev_count] = is_write;
    m->dev_count++;
}

/*
 * RAM is tested first, and that is safe ONLY because s5l8900_init() has already
 * refused to build a machine whose RAM aperture overlaps a device window (and
 * s5l8900_add_stub() refuses a window inside RAM). Under that invariant "RAM
 * first" and "device first" route identically, so the cheap test can go first.
 *
 * Do not relax the invariant and leave this ordering. It used to be the case
 * that RAM won by accident rather than by proof, and at -R 512 the DRAM window
 * reached 0x28000000 and swallowed the NOR: every NOR read returned RAM-disk
 * bytes, no fault, no log, nothing to find.
 */
static uint32_t bus_read(void *ctx, uint32_t addr, unsigned bytes) {
    s5l8900_t *m = ctx;

    if (in_ram(m, addr, bytes)) {
        uint32_t v = 0;
        memcpy(&v, &m->ram[addr - m->ram_base], bytes);   /* little-endian host */
        return v;
    }
    uint32_t v;
    if ((bytes == 1u || bytes == 2u || bytes == 4u) && (addr & 3u) == 0u &&
        in_dev(addr, bytes, S5L8900_UART0_BASE)) {
        v = s5l_uart_read(&m->uart0, addr - S5L8900_UART0_BASE);
    } else if (mmio_word(addr, bytes, S5L8900_VIC0_BASE, S5L8900_DEV_SIZE)) {
        uint32_t off = addr - S5L8900_VIC0_BASE;
        /* VIC0's VICADDRESS surfaces its own sources first, then daisy-chains
         * to VIC1 (global sources 32-63) — the standard PL192 cascade, and how
         * the driver reads a single global 0-63 source number from VIC0. */
        if (off == VIC_VECTADDR) {
            v = s5l_vic_vectaddr(&m->vic[0], 0);
            if (!v) v = s5l_vic_vectaddr(&m->vic[1], 32);
        } else {
            v = s5l_vic_read(&m->vic[0], off);
        }
    } else if (mmio_word(addr, bytes, S5L8900_VIC1_BASE, S5L8900_DEV_SIZE)) {
        uint32_t off = addr - S5L8900_VIC1_BASE;
        if (off == VIC_VECTADDR) v = s5l_vic_vectaddr(&m->vic[1], 32);
        else                     v = s5l_vic_read(&m->vic[1], off);
    } else if (mmio_word(addr, bytes, S5L8900_CLCD_BASE, S5L8900_DEV_SIZE)) {
        v = s5l_clcd_read(&m->clcd, addr - S5L8900_CLCD_BASE);
    } else if (in_timer(addr, bytes)) {
        v = s5l_timer_read(&m->timer, addr - S5L8900_TIMER_BASE);
    } else if (in_power(addr, bytes)) {
        v = s5l_power_read(&m->power, addr - S5L8900_POWER_BASE);
    } else if ((bytes == 1u || bytes == 2u || bytes == 4u) &&
               in_window(addr, bytes, S5L8900_NOR_BASE, m->nor.size)) {
        v = s5l_nor_read(&m->nor, addr - S5L8900_NOR_BASE, bytes);
    } else {
        s5l_stub_t *s = find_stub(m, addr, bytes);
        if (!s) {
            m->unmapped_reads++;
            note_unmapped(m, addr);
            note_device(m, addr, 0, false);
            return 0;
        }
        s->reads++;
        v = stub_read(s, addr, bytes);
    }
    note_device(m, addr, v, false);
    return v;
}

static void bus_write(void *ctx, uint32_t addr, uint32_t val, unsigned bytes) {
    s5l8900_t *m = ctx;

    if (in_ram(m, addr, bytes)) {
        memcpy(&m->ram[addr - m->ram_base], &val, bytes);
        return;
    }
    if ((bytes == 1u || bytes == 2u || bytes == 4u) && (addr & 3u) == 0u &&
        in_dev(addr, bytes, S5L8900_UART0_BASE)) {
        note_device(m, addr, val, true);
        s5l_uart_write(&m->uart0, addr - S5L8900_UART0_BASE, val);
        return;
    }
    if (mmio_word(addr, bytes, S5L8900_VIC0_BASE, S5L8900_DEV_SIZE)) {
        s5l_vic_write(&m->vic[0], addr - S5L8900_VIC0_BASE, val);
        return;
    }
    if (mmio_word(addr, bytes, S5L8900_VIC1_BASE, S5L8900_DEV_SIZE)) {
        s5l_vic_write(&m->vic[1], addr - S5L8900_VIC1_BASE, val);
        return;
    }
    if (mmio_word(addr, bytes, S5L8900_CLCD_BASE, S5L8900_DEV_SIZE)) {
        note_device(m, addr, val, true);
        s5l_clcd_write(&m->clcd, addr - S5L8900_CLCD_BASE, val);
        return;
    }
    if (in_timer(addr, bytes)) {
        s5l_timer_write(&m->timer, addr - S5L8900_TIMER_BASE, val);
        return;
    }
    if (in_power(addr, bytes)) {
        note_device(m, addr, val, true);
        s5l_power_write(&m->power, addr - S5L8900_POWER_BASE, val);
        return;
    }
    if ((bytes == 1u || bytes == 2u || bytes == 4u) &&
        in_window(addr, bytes, S5L8900_NOR_BASE, m->nor.size)) {
        /* Guest writes program the flash (bits can only be cleared). Note this
         * is a simplification: on real hardware the NOR is SPI-attached and is
         * programmed through controller commands rather than by storing to a
         * memory window. Modelling it as a direct write keeps the path a guest
         * payload needs — persisting itself into NOR, as an untethered
         * jailbreak does — without a full SPI protocol model. */
        s5l_nor_write(&m->nor, addr - S5L8900_NOR_BASE, val, bytes);
        return;
    }
    {
        s5l_stub_t *s = find_stub(m, addr, bytes);
        if (s) {
            s->writes++;
            note_device(m, addr, val, true);
            stub_write(s, addr, val, bytes);
            return;
        }
    }
    m->unmapped_writes++;
    note_unmapped(m, addr);
    note_device(m, addr, val, true);
}

static uint32_t r32(void *c, uint32_t a) { return bus_read(c, a, 4); }
static uint16_t r16(void *c, uint32_t a) { return (uint16_t)bus_read(c, a, 2); }
static uint8_t  r8 (void *c, uint32_t a) { return (uint8_t) bus_read(c, a, 1); }
static void w32(void *c, uint32_t a, uint32_t v) { bus_write(c, a, v, 4); }
static void w16(void *c, uint32_t a, uint16_t v) { bus_write(c, a, v, 2); }
static void w8 (void *c, uint32_t a, uint8_t  v) { bus_write(c, a, v, 1); }

/*
 * Complete one ARM1176 Wait For Interrupt operation without manufacturing CPU
 * work.  Only the timer and CLCD currently advance autonomously, so the first
 * edge either can route through the VIC is the earliest point at which this
 * model can wake the core.  Advancing farther would coalesce guest-visible
 * work across an interrupt; advancing less would merely replace the kernel's
 * idle loop with a host idle loop.
 */
static bool machine_wait_for_interrupt(void *ctx) {
    s5l8900_t *m = ctx;
    if (!m) return false;

    /* A line that is already high completes WFI even when CPSR masks it.  This
     * check precedes the controller refresh so an externally injected CPU line
     * is not lost. */
    if (m->cpu.irq_line || m->cpu.fiq_line) return true;

    /* Guest writes can change a VIC enable/mask or acknowledge a source after
     * the preceding instruction's tick.  Refresh level outputs at zero elapsed
     * time before deciding whether a future edge is necessary. */
    s5l8900_tick(m, 0);
    if (m->cpu.irq_line || m->cpu.fiq_line) return true;

    bool have_edge = false;
    uint32_t edge_tb = 0;
    const uint32_t timer_bit = 1u << S5L8900_IRQ_TIMER;
    const uint32_t clcd_bit  = 1u << S5L8900_IRQ_CLCD;

    if ((m->vic[0].enable & timer_bit) != 0u &&
        (m->timer.t4_state & TIMER4_STATE_START) != 0u) {
        uint32_t until_timer = m->timer.t4_value
                             ? m->timer.t4_value : m->timer.t4_count;
        if (until_timer != 0u) {
            edge_tb = until_timer;
            have_edge = true;
        }
    }

    if ((m->vic[0].enable & clcd_bit) != 0u &&
        (m->clcd.intmask & CLCD_INT_FRAME) != 0u &&
        m->clcd.scanning && m->clcd.frame_ticks != 0u) {
        /* frame_accum is normally strictly below frame_ticks.  If a malformed
         * in-memory caller violates that invariant, one tick is the only safe
         * boundary: the CLCD normalizes it and asserts the frame latch there. */
        uint32_t until_frame = m->clcd.frame_accum < m->clcd.frame_ticks
                             ? m->clcd.frame_ticks - m->clcd.frame_accum
                             : 1u;
        if (!have_edge || until_frame < edge_tb) {
            edge_tb = until_frame;
            have_edge = true;
        }
    }

    /* With no enabled autonomous source there is nothing safe to fast-forward
     * to.  Return to the interpreter's documented no-op fallback rather than
     * hanging the host forever or inventing an interrupt. */
    if (!have_edge || edge_tb == 0u) return false;

    uint64_t cpu_ticks;
    if (m->cpu_hz && m->tb_hz) {
        /* s5l8900_tick's integer converter can cross at most one timebase edge
         * per CPU tick only in this direction.  Refuse unusual inverted or
         * corrupt ratios instead of jumping past the earliest edge. */
        if (m->tb_hz > m->cpu_hz || m->tb_accum >= m->cpu_hz) return false;
        uint64_t need = (uint64_t)edge_tb * m->cpu_hz - m->tb_accum;
        cpu_ticks = need / m->tb_hz + (need % m->tb_hz != 0u);
    } else {
        /* Existing zero-clock fallback: s5l8900_tick treats its input as raw
         * timebase ticks when either advertised rate is zero. */
        cpu_ticks = edge_tb;
    }

    /* The public tick API is 32-bit.  Split a very long wait without changing
     * the exact fractional accumulator; no selected wake edge occurs before
     * the total derived above. */
    while (cpu_ticks > UINT32_MAX) {
        s5l8900_tick(m, UINT32_MAX);
        cpu_ticks -= UINT32_MAX;
        if (m->cpu.irq_line || m->cpu.fiq_line) return true;
    }
    if (cpu_ticks) s5l8900_tick(m, (uint32_t)cpu_ticks);
    return m->cpu.irq_line || m->cpu.fiq_line;
}

/* ----------------------------------------------------------- lifecycle --- */

bool s5l8900_init(s5l8900_t *m, uint32_t ram_base, uint32_t ram_size) {
    if (!m) return false;
    memset(m, 0, sizeof *m);

    /*
     * Refuse an aliasing configuration before allocating anything.
     *
     * This is the whole routing contract in three lines (soc.h explains why it
     * lives here rather than in bus_read). A machine whose DRAM window covers a
     * device window can only ever be one of two wrong things: a device that
     * cannot be reached, or a hole in the DRAM bank the guest was promised.
     * Neither is worth having, and both are invisible at run time, so the
     * machine simply does not exist.
     */
    if (!ram_size || (uint64_t)ram_base + ram_size > 0x100000000ull ||
        s5l8900_ram_conflict(ram_base, ram_size)) return false;

    m->ram = calloc(ram_size, 1);
    if (!m->ram) return false;
    m->ram_base = ram_base;
    m->ram_size = ram_size;
    m->cpu_hz   = S5L8900_CPU_HZ;
    m->tb_hz    = S5L8900_TB_HZ;

    s5l_uart_reset(&m->uart0);
    for (unsigned i = 0; i < S5L8900_VIC_COUNT; i++) s5l_vic_reset(&m->vic[i]);
    s5l_timer_reset(&m->timer);
    s5l_power_reset(&m->power);
    s5l_clcd_reset(&m->clcd);
    /* Refresh in the guest's own time: the CLCD is ticked with timebase ticks,
     * so the period is expressed in them. */
    m->clcd.frame_ticks = m->tb_hz / S5L_CLCD_REFRESH_HZ;
    if (!s5l_nor_init(&m->nor, S5L8900_NOR_SIZE)) { free(m->ram); m->ram = NULL; return false; }

    /*
     * Peripheral windows we have identified but not modelled. Each base was
     * resolved twice — from the VA the guest's own driver printed, walked
     * through its live page tables, and from the shipped device tree's arm-io
     * ranges (child + 0x38000000) — and the two agree. All are low-traffic and
     * none blocks the boot today; they are declared so their traffic is named
     * and stored instead of reading back as the zero an unmapped access
     * returns. See s5l_stub_t for why a stub is honest here and where the line
     * is that turns one into a real device model.
     *
     * A failure to declare one is not fatal but must not be silent, so the
     * result is folded into a counter the caller can see.
     */
    {
        static const struct { uint32_t base, size; const char *name; } STUBS[] = {
            /* AppleS5L8900XClockController _ccBaseAddress. */
            { S5L8900_CLOCK_BASE,  S5L8900_DEV_SIZE,   "clkrstgen" },
            /* _miuBaseAddress, the clock controller's second reg range.
             * Offsets 0x008 and 0x404 are the ones actually touched. */
            { S5L8900_MIU_BASE,    S5L8900_DEV_SIZE,   "miu"       },
            /* Offset 0x320 (FSEL) is the one actually touched. */
            { S5L8900_GPIO_BASE,   S5L8900_DEV_SIZE,   "gpio"      },
            { S5L8900_EDGEIC_BASE, S5L8900_DEV_SIZE,   "edgeic"    },
            /* The upper part of the power page — power.c owns 0x00-0x7f and
             * this is a different block with a different driver. */
            { S5L8900_GPIOIC_BASE, S5L8900_GPIOIC_SIZE, "gpioic"   },
        };
        for (unsigned i = 0; i < sizeof STUBS / sizeof STUBS[0]; i++)
            if (!s5l8900_add_stub(m, STUBS[i].base, STUBS[i].size, STUBS[i].name))
                m->stub_declare_failures++;
    }

    m->bus.ctx = m;
    m->bus.read32 = r32; m->bus.read16 = r16; m->bus.read8 = r8;
    m->bus.write32 = w32; m->bus.write16 = w16; m->bus.write8 = w8;
    m->bus.wait_for_interrupt = machine_wait_for_interrupt;

    arm_reset(&m->cpu, &m->bus);
    return true;
}

void s5l8900_free(s5l8900_t *m) {
    if (!m) return;
    for (unsigned i = 0; i < m->stub_count; i++) {
        free(m->stubs[i].regs);
        m->stubs[i].regs = NULL;
    }
    m->stub_count = 0;
    free(m->ram);
    m->ram = NULL;
    s5l_nor_free(&m->nor);
}

void s5l8900_load(s5l8900_t *m, uint32_t addr, const void *data, size_t len) {
    if (!m || !data || !len) return;
    /* Check before narrowing: a >4 GiB length must not truncate into range. */
    if (len > 0xffffffffu) return;
    if (!in_ram(m, addr, (uint32_t)len)) return;
    memcpy(&m->ram[addr - m->ram_base], data, len);
}

void s5l8900_tick(s5l8900_t *m, uint32_t ticks) {
    /*
     * Convert elapsed emulated CPU-clock ticks into timebase ticks at the
     * guest's own CPU:timebase ratio, carrying the remainder so it stays exact
     * rather than drifting. Active execution contributes one such tick per
     * retired instruction; WFI contributes elapsed idle ticks without
     * changing cpu.cycles. Feeding ticks straight into the timebase runs guest
     * time ~68x fast and livelocks the kernel's decrementer.
     */
    uint32_t tb = ticks;
    if (m->cpu_hz && m->tb_hz) {
        m->tb_accum += (uint64_t)ticks * m->tb_hz;
        tb = (uint32_t)(m->tb_accum / m->cpu_hz);
        m->tb_accum %= m->cpu_hz;
    }

    /* Devices advance, then the controllers recompute what the CPU sees. */
    bool timer_irq = s5l_timer_tick(&m->timer, tb);
    s5l_vic_set_line(&m->vic[0], S5L8900_IRQ_TIMER, timer_irq);

    bool clcd_irq = s5l_clcd_tick(&m->clcd, tb);
    s5l_vic_set_line(&m->vic[0], S5L8900_IRQ_CLCD, clcd_irq);

    /*
     * BOTH VICs drive the CPU.
     *
     * Only VIC0 used to, which was fine while VIC0 was the only one mapped. It
     * is not defensible now: the device tree numbers interrupts flat across the
     * pair — /arm-io/gpio lists lines 0x20 and 0x21, /arm-io/wdt 0x33,
     * /arm-io/sdio 0x2a, /arm-io/edgeic 0x23 and 0x29 — and every one of those
     * is past VIC0's 32 lines, so it lives on VIC1. Leaving VIC1's outputs
     * disconnected would mean the watchdog, the SD controller and the GPIO
     * interrupt controller could be correctly programmed, correctly asserted,
     * and still never reach the CPU: a silent failure, and precisely the kind
     * this core exists to avoid.
     *
     * OR-ing the two is the standard PL192 cascade. Nothing asserts a VIC1 line
     * today, so this changes no behaviour now; it removes a trap for the next
     * device that needs a line above 31.
     */
    bool irq = false, fiq = false;
    for (unsigned i = 0; i < S5L8900_VIC_COUNT; i++) {
        irq |= s5l_vic_irq(&m->vic[i]);
        fiq |= s5l_vic_fiq(&m->vic[i]);
    }
    m->cpu.irq_line = irq;
    m->cpu.fiq_line = fiq;
}

unsigned s5l8900_run(s5l8900_t *m, unsigned max_steps, arm_status_t *status) {
    arm_status_t st = ARM_OK;
    unsigned n = 0;
    for (; n < max_steps; n++) {
        st = arm_step(&m->cpu);
        if (st != ARM_OK) break;
        s5l8900_tick(m, 1);
    }
    if (status) *status = st;
    return n;
}

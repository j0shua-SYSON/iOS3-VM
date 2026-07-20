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
static inline bool in_dev(uint32_t a, uint32_t base) {
    return a >= base && (uint64_t)a < (uint64_t)base + S5L8900_DEV_SIZE;
}
/* The timer is the one peripheral that does not fit the uniform 4 KB window:
 * its interrupt-status alias sits at offset 0x10000. */
static inline bool in_power(uint32_t a) {
    return a >= S5L8900_POWER_BASE &&
           (uint64_t)a < (uint64_t)S5L8900_POWER_BASE + S5L8900_POWER_SIZE;
}
static inline bool in_timer(uint32_t a) {
    return a >= S5L8900_TIMER_BASE &&
           (uint64_t)a < (uint64_t)S5L8900_TIMER_BASE + S5L8900_TIMER_SIZE;
}

/* ------------------------------------------------------- stub windows --- */

bool s5l8900_add_stub(s5l8900_t *m, uint32_t base, uint32_t size,
                      const char *name) {
    if (m->stub_count >= S5L_STUB_MAX || !size) return false;
    /* Refuse to shadow an existing window: a stub that quietly overlays a real
     * device model would be far harder to notice than a rejected call. */
    for (unsigned i = 0; i < m->stub_count; i++) {
        uint64_t a0 = m->stubs[i].base, a1 = a0 + m->stubs[i].size;
        uint64_t b0 = base,             b1 = b0 + size;
        if (b0 < a1 && a0 < b1) return false;
    }
    uint32_t nregs = (size + 3u) / 4u;
    uint32_t *regs = calloc(nregs, sizeof *regs);
    if (!regs) return false;

    s5l_stub_t *s = &m->stubs[m->stub_count++];
    memset(s, 0, sizeof *s);
    s->base = base; s->size = size; s->name = name;
    s->regs = regs; s->nregs = nregs;
    return true;
}

static s5l_stub_t *find_stub(s5l8900_t *m, uint32_t a) {
    for (unsigned i = 0; i < m->stub_count; i++)
        if (a >= m->stubs[i].base &&
            (uint64_t)a < (uint64_t)m->stubs[i].base + m->stubs[i].size)
            return &m->stubs[i];
    return NULL;
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

static uint32_t bus_read(void *ctx, uint32_t addr, unsigned bytes) {
    s5l8900_t *m = ctx;

    if (in_ram(m, addr, bytes)) {
        uint32_t v = 0;
        memcpy(&v, &m->ram[addr - m->ram_base], bytes);   /* little-endian host */
        return v;
    }
    uint32_t v;
    if (in_dev(addr, S5L8900_UART0_BASE)) {
        v = s5l_uart_read(&m->uart0, addr - S5L8900_UART0_BASE);
    } else if (in_dev(addr, S5L8900_VIC0_BASE)) {
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
    } else if (in_dev(addr, S5L8900_VIC1_BASE)) {
        uint32_t off = addr - S5L8900_VIC1_BASE;
        if (off == VIC_VECTADDR) v = s5l_vic_vectaddr(&m->vic[1], 32);
        else                     v = s5l_vic_read(&m->vic[1], off);
    } else if (in_dev(addr, S5L8900_CLCD_BASE)) {
        v = s5l_clcd_read(&m->clcd, addr - S5L8900_CLCD_BASE);
    } else if (in_timer(addr)) {
        v = s5l_timer_read(&m->timer, addr - S5L8900_TIMER_BASE);
    } else if (in_power(addr)) {
        v = s5l_power_read(&m->power, addr - S5L8900_POWER_BASE);
    } else if (addr >= S5L8900_NOR_BASE &&
               (uint64_t)addr < (uint64_t)S5L8900_NOR_BASE + m->nor.size) {
        v = s5l_nor_read(&m->nor, addr - S5L8900_NOR_BASE, bytes);
    } else {
        s5l_stub_t *s = find_stub(m, addr);
        if (!s) {
            m->unmapped_reads++;
            note_unmapped(m, addr);
            note_device(m, addr, 0, false);
            return 0;
        }
        uint32_t off = (addr - s->base) >> 2;
        s->reads++;
        if (off < s->nregs) v = s->regs[off];
        else { s->oob++; v = 0; }
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
    if (in_dev(addr, S5L8900_UART0_BASE)) {
        note_device(m, addr, val, true);
        s5l_uart_write(&m->uart0, addr - S5L8900_UART0_BASE, val);
        return;
    }
    if (in_dev(addr, S5L8900_VIC0_BASE)) {
        s5l_vic_write(&m->vic[0], addr - S5L8900_VIC0_BASE, val);
        return;
    }
    if (in_dev(addr, S5L8900_VIC1_BASE)) {
        s5l_vic_write(&m->vic[1], addr - S5L8900_VIC1_BASE, val);
        return;
    }
    if (in_dev(addr, S5L8900_CLCD_BASE)) {
        note_device(m, addr, val, true);
        s5l_clcd_write(&m->clcd, addr - S5L8900_CLCD_BASE, val);
        return;
    }
    if (in_timer(addr)) {
        s5l_timer_write(&m->timer, addr - S5L8900_TIMER_BASE, val);
        return;
    }
    if (in_power(addr)) {
        note_device(m, addr, val, true);
        s5l_power_write(&m->power, addr - S5L8900_POWER_BASE, val);
        return;
    }
    if (addr >= S5L8900_NOR_BASE &&
        (uint64_t)addr < (uint64_t)S5L8900_NOR_BASE + m->nor.size) {
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
        s5l_stub_t *s = find_stub(m, addr);
        if (s) {
            uint32_t off = (addr - s->base) >> 2;
            s->writes++;
            note_device(m, addr, val, true);
            if (off < s->nregs) s->regs[off] = val;
            else s->oob++;
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

/* ----------------------------------------------------------- lifecycle --- */

bool s5l8900_init(s5l8900_t *m, uint32_t ram_base, uint32_t ram_size) {
    memset(m, 0, sizeof *m);
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

    arm_reset(&m->cpu, &m->bus);
    return true;
}

void s5l8900_free(s5l8900_t *m) {
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
    /* Check before narrowing: a >4 GiB length must not truncate into range. */
    if (len > 0xffffffffu) return;
    if (!in_ram(m, addr, (uint32_t)len)) return;
    memcpy(&m->ram[addr - m->ram_base], data, len);
}

void s5l8900_tick(s5l8900_t *m, uint32_t ticks) {
    /*
     * Convert retired instructions into timebase ticks at the guest's own
     * CPU:timebase ratio, carrying the remainder so it stays exact rather than
     * drifting. Feeding instructions straight in runs guest time ~68x fast and
     * livelocks the kernel's decrementer (see S5L8900_CPU_HZ).
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

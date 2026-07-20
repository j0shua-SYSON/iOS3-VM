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

/* ------------------------------------------------------------- reads --- */

static uint32_t bus_read(void *ctx, uint32_t addr, unsigned bytes) {
    s5l8900_t *m = ctx;

    if (in_ram(m, addr, bytes)) {
        uint32_t v = 0;
        memcpy(&v, &m->ram[addr - m->ram_base], bytes);   /* little-endian host */
        return v;
    }
    if (in_dev(addr, S5L8900_UART0_BASE))
        return s5l_uart_read(&m->uart0, addr - S5L8900_UART0_BASE);
    if (in_dev(addr, S5L8900_VIC0_BASE))
        return s5l_vic_read(&m->vic0, addr - S5L8900_VIC0_BASE);
    if (in_dev(addr, S5L8900_TIMER_BASE))
        return s5l_timer_read(&m->timer, addr - S5L8900_TIMER_BASE);
    if (addr >= S5L8900_NOR_BASE &&
        (uint64_t)addr < (uint64_t)S5L8900_NOR_BASE + m->nor.size)
        return s5l_nor_read(&m->nor, addr - S5L8900_NOR_BASE, bytes);

    m->unmapped_reads++;
    return 0;
}

static void bus_write(void *ctx, uint32_t addr, uint32_t val, unsigned bytes) {
    s5l8900_t *m = ctx;

    if (in_ram(m, addr, bytes)) {
        memcpy(&m->ram[addr - m->ram_base], &val, bytes);
        return;
    }
    if (in_dev(addr, S5L8900_UART0_BASE)) {
        s5l_uart_write(&m->uart0, addr - S5L8900_UART0_BASE, val);
        return;
    }
    if (in_dev(addr, S5L8900_VIC0_BASE)) {
        s5l_vic_write(&m->vic0, addr - S5L8900_VIC0_BASE, val);
        return;
    }
    if (in_dev(addr, S5L8900_TIMER_BASE)) {
        s5l_timer_write(&m->timer, addr - S5L8900_TIMER_BASE, val);
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
    m->unmapped_writes++;
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

    s5l_uart_reset(&m->uart0);
    s5l_vic_reset(&m->vic0);
    s5l_timer_reset(&m->timer);
    if (!s5l_nor_init(&m->nor, S5L8900_NOR_SIZE)) { free(m->ram); m->ram = NULL; return false; }

    m->bus.ctx = m;
    m->bus.read32 = r32; m->bus.read16 = r16; m->bus.read8 = r8;
    m->bus.write32 = w32; m->bus.write16 = w16; m->bus.write8 = w8;

    arm_reset(&m->cpu, &m->bus);
    return true;
}

void s5l8900_free(s5l8900_t *m) {
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
    /* Devices advance, then the controller recomputes what the CPU sees. */
    bool timer_irq = s5l_timer_tick(&m->timer, ticks);
    s5l_vic_set_line(&m->vic0, S5L8900_IRQ_TIMER, timer_irq);

    m->cpu.irq_line = s5l_vic_irq(&m->vic0);
    m->cpu.fiq_line = s5l_vic_fiq(&m->vic0);
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

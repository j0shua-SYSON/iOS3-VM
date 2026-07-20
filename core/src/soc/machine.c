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

static inline bool in_ram(const s5l8900_t *m, uint32_t a, uint32_t len) {
    return a >= m->ram_base && (a - m->ram_base) + len <= m->ram_size;
}
static inline bool in_dev(uint32_t a, uint32_t base) {
    return a >= base && a < base + S5L8900_DEV_SIZE;
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

    m->bus.ctx = m;
    m->bus.read32 = r32; m->bus.read16 = r16; m->bus.read8 = r8;
    m->bus.write32 = w32; m->bus.write16 = w16; m->bus.write8 = w8;

    arm_reset(&m->cpu, &m->bus);
    return true;
}

void s5l8900_free(s5l8900_t *m) {
    free(m->ram);
    m->ram = NULL;
}

void s5l8900_load(s5l8900_t *m, uint32_t addr, const void *data, size_t len) {
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

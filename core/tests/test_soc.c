/*
 * iOS3-VM — S5L8900 machine tests.
 *
 * The headline test runs a hand-assembled bare-metal ARM payload on the
 * emulated SoC and reads back what it printed over the UART. This is the first
 * point where guest code produces observable output — the same channel iBoot
 * and XNU will use later.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ------------------------------------------------------------------------ */

static void test_ram_readback(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");
    uint32_t v = 0xdeadbeef;
    s5l8900_load(&m, 0x100, &v, 4);
    CHECK(m.bus.read32(m.bus.ctx, 0x100) == 0xdeadbeefu,
          "ram readback = %08x", m.bus.read32(m.bus.ctx, 0x100));
    s5l8900_free(&m);
}

static void test_uart_status_is_ready(void) {
    /* A guest polling UTRSTAT must see the transmitter ready, or it spins. */
    s5l8900_t m;
    s5l8900_init(&m, 0, 1u << 16);
    uint32_t st = m.bus.read32(m.bus.ctx, S5L8900_UART0_BASE + UART_UTRSTAT);
    CHECK((st & (1u << 2)) != 0, "UTRSTAT=%08x expect TX-empty set", st);
    s5l8900_free(&m);
}

static void test_bounds_check_cannot_overflow(void) {
    /* Regression: a 32-bit "(addr - base) + len <= size" wraps for addresses
     * near the top of the address space, letting a guest access index far
     * outside the RAM allocation. The guest controls every address, so this
     * must be rejected and merely counted as unmapped. */
    s5l8900_t m;
    s5l8900_init(&m, 0, 1u << 20);
    (void)m.bus.read32(m.bus.ctx, 0xfffffffeu);
    CHECK(m.unmapped_reads == 1, "0xfffffffe read should be unmapped, not accepted");
    m.bus.write32(m.bus.ctx, 0xfffffffcu, 0xdeadbeefu);
    CHECK(m.unmapped_writes == 1, "0xfffffffc write should be unmapped, not accepted");
    /* The last legal word must still work. */
    m.bus.write32(m.bus.ctx, (1u << 20) - 4u, 0x12345678u);
    CHECK(m.bus.read32(m.bus.ctx, (1u << 20) - 4u) == 0x12345678u,
          "the final in-range word should still be accessible");
    CHECK(m.unmapped_writes == 1, "in-range write was wrongly rejected");
    s5l8900_free(&m);
}

static void test_unmapped_access_counted(void) {
    /* Accesses outside the memory map are counted, not silently swallowed. */
    s5l8900_t m;
    s5l8900_init(&m, 0, 1u << 16);
    (void)m.bus.read32(m.bus.ctx, 0x70000000u);
    CHECK(m.unmapped_reads == 1, "unmapped_reads=%llu expect 1",
          (unsigned long long)m.unmapped_reads);
    s5l8900_free(&m);
}

/*
 * The bare-metal payload. Hand-assembled ARM that loads the UART base from a
 * literal and pushes "HI\n" out the transmit register, then spins.
 *
 *   00: LDR r0,[pc,#24]     ; r0 = UART0 base (literal at 0x20)
 *   04: MOV r1,#'H'
 *   08: STR r1,[r0,#0x20]   ; UTXH
 *   0c: MOV r1,#'I'
 *   10: STR r1,[r0,#0x20]
 *   14: MOV r1,#'\n'
 *   18: STR r1,[r0,#0x20]
 *   1c: B .                 ; park
 *   20: .word S5L8900_UART0_BASE
 */
static void test_bare_metal_uart_hello(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    const uint32_t payload[] = {
        0xe59f0018u,            /* LDR r0,[pc,#24]   */
        0xe3a01048u,            /* MOV r1,#0x48 'H'  */
        0xe5801020u,            /* STR r1,[r0,#0x20] */
        0xe3a01049u,            /* MOV r1,#0x49 'I'  */
        0xe5801020u,            /* STR r1,[r0,#0x20] */
        0xe3a0100au,            /* MOV r1,#0x0a '\n' */
        0xe5801020u,            /* STR r1,[r0,#0x20] */
        0xeafffffeu,            /* B .               */
        S5L8900_UART0_BASE      /* literal           */
    };
    s5l8900_load(&m, 0, payload, sizeof payload);

    arm_status_t st = ARM_OK;
    m.cpu.r[15] = 0;
    unsigned n = s5l8900_run(&m, 32, &st);

    CHECK(st == ARM_OK, "status=%d expect ARM_OK after %u steps", (int)st, n);
    m.uart0.tx[m.uart0.tx_len] = '\0';
    CHECK(strcmp(m.uart0.tx, "HI\n") == 0,
          "uart output = \"%s\" expect \"HI\\n\"", m.uart0.tx);
    CHECK(m.unmapped_writes == 0, "unexpected unmapped writes: %llu",
          (unsigned long long)m.unmapped_writes);

    printf("  [guest said] %s", m.uart0.tx);
    s5l8900_free(&m);
}

/*
 * The free-running counter is mach_absolute_time(). It must advance even when
 * no timer is armed: a counter that only runs while timer 4 is started reads
 * zero for the whole of early boot, and every delay loop in the kernel then
 * waits forever on a clock that never moves.
 */
static void test_timebase_runs_without_a_timer(void) {
    s5l_timer_t t; s5l_timer_reset(&t);
    s5l_timer_tick(&t, 1000);
    uint32_t lo = s5l_timer_read(&t, TIMER_TICKSLOW);
    CHECK(lo == 1000, "timebase=%u expect 1000 with no timer armed", lo);

    /* And it must carry into the high word rather than wrapping silently. */
    s5l_timer_reset(&t);
    for (unsigned i = 0; i < 5; i++) s5l_timer_tick(&t, 0xfffffffful / 4u);
    uint32_t hi = s5l_timer_read(&t, TIMER_TICKSHIGH);
    CHECK(hi == 1, "timebase high=%u expect 1 after passing 2^32", hi);
}

static void test_timer_period_is_exact(void) {
    /* One interrupt per reload period, at a steady interval. Latching on both
     * the decrement-to-zero and reload-from-zero paths produced two interrupts
     * per period at intervals N, 1, N, 1, ... */
    s5l_timer_t t; s5l_timer_reset(&t);
    s5l_timer_write(&t, TIMER4_COUNTBUF, 4);
    s5l_timer_write(&t, TIMER4_STATE, TIMER4_STATE_START | TIMER4_STATE_UPDATE);
    unsigned expiries = 0, last = 0; int deltas_ok = 1;
    for (unsigned tick = 1; tick <= 20; tick++) {
        if (s5l_timer_tick(&t, 1)) {
            if (last && (tick - last) != 4) deltas_ok = 0;
            last = tick; expiries++;
            s5l_timer_write(&t, TIMER_IRQACK, TIMER4_IRQ_BITS);   /* acknowledge */
        }
    }
    CHECK(expiries == 5, "expiries=%u expect 5 in 20 ticks with count 4", expiries);
    CHECK(deltas_ok, "expiry intervals were not a steady 4 ticks");
}

/*
 * The acknowledge mask is not a free choice: the kernel's FIQ handler writes
 * exactly TIMER4_IRQ_BITS. If the latch holds any bit that write does not
 * clear, the line stays asserted, the handler re-enters immediately, and the
 * boot presents as a hang rather than as a scheduler tick.
 */
static void test_timer_ack_mask_matches_the_kernels(void) {
    s5l_timer_t t; s5l_timer_reset(&t);
    s5l_timer_write(&t, TIMER4_COUNTBUF, 1);
    s5l_timer_write(&t, TIMER4_STATE, TIMER4_STATE_START | TIMER4_STATE_UPDATE);
    CHECK(s5l_timer_tick(&t, 1), "timer should latch an interrupt at expiry");
    s5l_timer_write(&t, TIMER_IRQACK, TIMER4_IRQ_BITS);
    CHECK(s5l_timer_read(&t, TIMER_IRQLATCH) == 0,
          "latch=%08x expect fully cleared by the kernel's ack mask",
          s5l_timer_read(&t, TIMER_IRQLATCH));
}

/*
 * Guest time must advance at the guest's own CPU:timebase ratio, not once per
 * instruction. Running the timebase at instruction rate makes time pass ~68x
 * too fast relative to work done, so the kernel never finishes servicing one
 * decrementer deadline before the next is already past; it clamps to its
 * minimum and re-enters forever. That livelock burned 66% of a 200M-instruction
 * boot inside the FIQ handler before this ratio existed.
 */
static void test_timebase_runs_at_the_guest_ratio(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    const uint32_t insns = 412000;      /* one hundredth of a guest second */
    for (uint32_t i = 0; i < insns; i++) s5l8900_tick(&m, 1);

    uint32_t tb = m.bus.read32(m.bus.ctx, S5L8900_TIMER_BASE + TIMER_TICKSLOW);
    uint32_t want = (uint32_t)((uint64_t)insns * S5L8900_TB_HZ / S5L8900_CPU_HZ);
    CHECK(tb == want, "timebase=%u expect %u (%u insns at %u:%u)",
          tb, want, insns, S5L8900_CPU_HZ, S5L8900_TB_HZ);
    CHECK(tb != insns, "timebase must not advance once per instruction");

    /* The remainder must carry rather than be discarded: ticking one at a time
     * has to match ticking in one lump, or time drifts against itself. */
    s5l8900_t b;
    CHECK(s5l8900_init(&b, 0, 1u << 20), "machine init failed");
    s5l8900_tick(&b, insns);
    uint32_t lump = b.bus.read32(b.bus.ctx, S5L8900_TIMER_BASE + TIMER_TICKSLOW);
    CHECK(lump == tb, "lump=%u single-stepped=%u — remainder is being dropped",
          lump, tb);

    s5l8900_free(&m);
    s5l8900_free(&b);
}

/*
 * Stub windows are honest storage, not invented behaviour. The properties that
 * matter are that they read back what was written (rather than the 0 that an
 * unmapped read returns, which is what made a driver spin 3.9 million times),
 * that they are counted rather than silent, and above all that one can never
 * shadow a real device model.
 */
static void test_stub_window_stores_and_counts(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    CHECK(s5l8900_add_stub(&m, 0x3b000000u, 0x1000u, "unknown-3b0"),
          "declaring a stub window should succeed");
    /* The machine declares its own stubs at init, so this one is not index 0.
     * Look it up by name rather than by position — an index that silently
     * points at the wrong window would make every assertion below vacuous. */
    s5l_stub_t *mine = NULL;
    for (unsigned i = 0; i < m.stub_count; i++)
        if (!strcmp(m.stubs[i].name, "unknown-3b0")) mine = &m.stubs[i];
    CHECK(mine != NULL, "the stub just declared should be in the table");
    if (!mine) { s5l8900_free(&m); return; }

    /* Reads return what was written, not zero. */
    m.bus.write32(m.bus.ctx, 0x3b000010u, 0xa5a5a5a5u);
    CHECK(m.bus.read32(m.bus.ctx, 0x3b000010u) == 0xa5a5a5a5u,
          "stub read=%08x expect the value written",
          m.bus.read32(m.bus.ctx, 0x3b000010u));

    /* Stubbed traffic must not be counted as unmapped — it is accounted to the
     * window so it shows up by name in the report. */
    CHECK(m.unmapped_reads == 0 && m.unmapped_writes == 0,
          "stubbed access must not count as unmapped");
    CHECK(mine->reads == 1 && mine->writes == 1,
          "stub r=%llu w=%llu expect 1/1",
          (unsigned long long)mine->reads,
          (unsigned long long)mine->writes);

    /* The backing store must cover the WHOLE declared window, not a fixed
     * prefix. A 64-register array covered offsets 0x000-0x0FC while the two
     * registers actually measured on this SoC live at 0x320 (GPIO FSEL) and
     * 0x404 (CLOCK0 ADJ2) — so both were counted but not stored, and the stub
     * silently failed to be honest storage for exactly the registers that
     * mattered. */
    m.bus.write32(m.bus.ctx, 0x3b000320u, 0x0006070fu);
    CHECK(m.bus.read32(m.bus.ctx, 0x3b000320u) == 0x0006070fu,
          "off 0x320 read=%08x expect the value written (whole window backed)",
          m.bus.read32(m.bus.ctx, 0x3b000320u));
    CHECK(mine->oob == 0, "an in-window access must not count as oob");

    /* Past the declared window is a different matter: still counted, not
     * stored, so the shortfall stays visible instead of being pretended away. */
    (void)m.bus.read32(m.bus.ctx, 0x3b000000u + 0x1000u - 4u);
    CHECK(mine->oob == 0, "the final in-window word must still be backed");

    /* Overlap must be refused: silently shadowing a modelled device would be
     * far harder to notice than a rejected declaration. */
    CHECK(!s5l8900_add_stub(&m, 0x3b000800u, 0x1000u, "overlapping"),
          "an overlapping stub window must be refused");
    CHECK(!s5l8900_add_stub(&m, S5L8900_UART0_BASE, 0x1000u, "over-uart") ||
          m.bus.read32(m.bus.ctx, S5L8900_UART0_BASE + UART_UTRSTAT) & (1u << 2),
          "a stub must never take precedence over a real device model");

    s5l8900_free(&m);
}

static void test_mmio_width_alignment_and_window_edges(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");
    CHECK(s5l8900_add_stub(&m, 0x3b100000u, 8u, "lanes"),
          "small stub declaration failed");

    /* Stub storage is byte-addressable and little-endian, including an
     * unaligned halfword that spans two backing uint32_t values. */
    m.bus.write32(m.bus.ctx, 0x3b100000u, 0x11223344u);
    m.bus.write8(m.bus.ctx, 0x3b100001u, 0xaau);
    CHECK(m.bus.read32(m.bus.ctx, 0x3b100000u) == 0x1122aa44u,
          "byte write updated the wrong register lane");
    m.bus.write16(m.bus.ctx, 0x3b100003u, 0xbeefu);
    CHECK(m.bus.read16(m.bus.ctx, 0x3b100003u) == 0xbeefu,
          "unaligned cross-register halfword did not round-trip");

    /* An access must fit wholly inside its window. Starting in the last byte
     * is not permission to spill into the next physical region. */
    uint64_t before = m.unmapped_reads;
    (void)m.bus.read16(m.bus.ctx, 0x3b100007u);
    CHECK(m.unmapped_reads == before + 1u,
          "cross-boundary stub read was treated as mapped");
    before = m.unmapped_reads;
    (void)m.bus.read16(m.bus.ctx, S5L8900_NOR_BASE + S5L8900_NOR_SIZE - 1u);
    CHECK(m.unmapped_reads == before + 1u,
          "cross-boundary NOR read was treated as mapped");

    /* Register devices are 32-bit MMIO. A byte store must not accidentally
     * become a full-register write, while UART TX deliberately accepts STRB. */
    uint64_t writes = m.unmapped_writes;
    m.bus.write8(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u);
    CHECK(m.unmapped_writes == writes + 1u && m.vic[0].enable == 0,
          "narrow VIC write clobbered a word register");
    m.bus.write8(m.bus.ctx, S5L8900_UART0_BASE + UART_UTXH, 'X');
    CHECK(m.uart0.tx_len == 1 && m.uart0.tx[0] == 'X',
          "aligned UART byte transmit should remain supported");
    s5l8900_free(&m);
}

static void test_address_space_wrap_is_refused(void) {
    s5l8900_t m;
    CHECK(!s5l8900_init(&m, 0, 0), "zero-sized RAM was accepted");
    CHECK(!s5l8900_init(&m, 0xffffff00u, 0x200u),
          "RAM aperture wrapping past 4 GiB was accepted");

    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");
    CHECK(!s5l8900_add_stub(&m, 0xf0000000u, 0x20000000u, "wrap"),
          "stub window wrapping past 4 GiB was accepted");
    s5l8900_free(&m);
}

/*
 * The power controller is the block that stood between a booting kernel and a
 * booting OS. AppleS5L8900XPowerController::start writes the domains it wants
 * gated and then spins until STATE agrees:
 *
 *     write(OFFCTRL, 0x12fc);  do { s = read(STATE); } while ((s & 0x12fc) != 0x12fc);
 *
 * Unmodelled, STATE read 0 forever: 3,887,707 reads, about a quarter of a
 * 200M-instruction boot, and start() never returned. The property under test is
 * the coupling — that ONCTRL and OFFCTRL are the only things that move STATE,
 * with the polarity the driver's own gate routine uses.
 */
static void test_power_gate_state_tracks_onctrl_offctrl(void) {
    s5l_power_t p; s5l_power_reset(&p);

    /* Gate: write-1-to-SET. This is the exact sequence and mask the driver
     * uses, so passing this is passing the loop that wedged the boot. */
    s5l_power_write(&p, POWER_OFFCTRL, 0x12fcu);
    uint32_t st = s5l_power_read(&p, POWER_STATE);
    CHECK((st & 0x12fcu) == 0x12fcu,
          "STATE=%08x expect bits 0x12fc set after OFFCTRL — this is the "
          "condition AppleS5L8900XPowerController::start spins on", st);

    /* Ungate: write-1-to-CLEAR. Bit 9 is USB, gated then ungated around
     * AppleS5L8900XUSBPhy::start. */
    s5l_power_write(&p, POWER_ONCTRL, 1u << 9);
    st = s5l_power_read(&p, POWER_STATE);
    CHECK((st & (1u << 9)) == 0, "STATE=%08x expect bit 9 cleared by ONCTRL", st);
    CHECK((st & (1u << 12)) != 0,
          "ONCTRL must clear only the bits written (bit 12 was gated too)");

    /* STATE is read-only: it moves via ONCTRL/OFFCTRL and nothing else. A
     * storage stub would have "worked" here by accident and failed in the
     * boot, because the guest never writes STATE at all. */
    uint32_t before = s5l_power_read(&p, POWER_STATE);
    s5l_power_write(&p, POWER_STATE, 0xffffffffu);
    CHECK(s5l_power_read(&p, POWER_STATE) == before,
          "STATE must be read-only");

    /* Writes outside the 14 modelled domains must not set phantom bits. */
    s5l_power_reset(&p);
    s5l_power_write(&p, POWER_ONCTRL, 0xffffffffu);
    s5l_power_write(&p, POWER_OFFCTRL, 0xffffffffu);
    CHECK(s5l_power_read(&p, POWER_STATE) == S5L_POWER_DOMAIN_MASK,
          "STATE=%08x expect only the 14 real domains",
          s5l_power_read(&p, POWER_STATE));
}

/*
 * The peripheral windows the machine declares for itself. The property under
 * test is not "a stub exists" but that each one is at the address two
 * independent sources agree on — the device tree's arm-io ranges and a walk of
 * the guest's live page tables — and that declaring them all actually
 * succeeded. A refused declaration leaves the window unmapped, which reads back
 * as zero and is exactly the silent guess stubs exist to replace.
 */
static void test_machine_declares_its_known_windows(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");
    CHECK(m.stub_declare_failures == 0,
          "%u stub declarations were refused at init", m.stub_declare_failures);

    static const struct { uint32_t addr; const char *name; } WANT[] = {
        { S5L8900_CLOCK_BASE,  "clkrstgen" },
        { S5L8900_MIU_BASE,    "miu"       },
        { S5L8900_GPIO_BASE,   "gpio"      },
        { S5L8900_EDGEIC_BASE, "edgeic"    },
        { S5L8900_GPIOIC_BASE, "gpioic"    },
    };
    for (unsigned i = 0; i < sizeof WANT / sizeof WANT[0]; i++) {
        /* Reads must come back as storage, not as the unmapped-access zero. */
        m.bus.write32(m.bus.ctx, WANT[i].addr + 8u, 0xc0ffee00u + i);
        CHECK(m.bus.read32(m.bus.ctx, WANT[i].addr + 8u) == 0xc0ffee00u + i,
              "%s at 0x%08x did not read back what was written",
              WANT[i].name, WANT[i].addr);
    }
    CHECK(m.unmapped_reads == 0 && m.unmapped_writes == 0,
          "declared windows must not count as unmapped (r=%llu w=%llu)",
          (unsigned long long)m.unmapped_reads,
          (unsigned long long)m.unmapped_writes);

    /* The two registers we have actually measured on this SoC live past the
     * first 256 bytes; check them specifically, because a short backing store
     * once swallowed exactly these. */
    m.bus.write32(m.bus.ctx, S5L8900_GPIO_BASE + 0x320u, 0x0006070fu);
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_GPIO_BASE + 0x320u) == 0x0006070fu,
          "GPIO FSEL at +0x320 must be backed");
    m.bus.write32(m.bus.ctx, S5L8900_MIU_BASE + 0x404u, 0x12345678u);
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_MIU_BASE + 0x404u) == 0x12345678u,
          "MIU +0x404 must be backed");

    /* The gpioic stub must not shadow the power controller: they share a page
     * and only 0x00-0x7f belongs to power. */
    m.bus.write32(m.bus.ctx, S5L8900_POWER_BASE + POWER_OFFCTRL, 0x12fcu);
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_POWER_BASE + POWER_STATE) == 0x12fcu,
          "the gpioic stub must not take over the power controller's half");

    s5l8900_free(&m);
}

/* ------------------------------------------------- RAM vs device routing ---
 *
 * The bug these four tests exist for: at -R 512 the DRAM window
 * 0x08000000..0x28000000 reached over the NOR, bus_read tested RAM first and
 * returned before it ever consulted a device, so every NOR read came back as
 * whatever byte of the RAM disk happened to live there. No fault, no log, no
 * counter — and NOR is where an era-appropriate untether (24kpwn) has to
 * persist, so "we boot fine at -R 512" and "NOR works" could not both be true.
 */

/*
 * THE HEADLINE: at the geometry the real boot uses, a NOR read is NOR's bytes.
 *
 * "It read back what I programmed" is not on its own proof — RAM would do that
 * too. What proves the read reached the flash MODEL is a flash-only behaviour:
 * programming can clear bits but never set one back to 1. RAM would happily
 * take 0xffffffff; NOR must refuse it and keep the old value.
 */
static void test_nor_reads_are_nor_at_the_boot_ram_size(void) {
    /* Exactly what `bootkernel -R 512` builds: DRAM 0x08000000..0x28000000. */
    const uint32_t base = S5L8900_SDRAM_BASE, size = 512u << 20;
    s5l8900_t m;
    CHECK(s5l8900_init(&m, base, size),
          "the -R 512 geometry must still be constructible");
    if (!m.ram) return;

    /* Distinctive "RAM disk" bytes in the last word of DRAM, immediately below
     * the NOR window: if routing ever slips by one window again, this is the
     * value a NOR read would come back with. */
    const uint32_t ramdisk = 0xa5a5a5a5u;
    m.bus.write32(m.bus.ctx, base + size - 4u, ramdisk);
    CHECK(m.bus.read32(m.bus.ctx, base + size - 4u) == ramdisk,
          "the last word of DRAM must still be RAM");

    /* Known bytes in the flash, laid down as a factory flasher would. */
    const uint32_t flashed = 0x0f0f0f0fu;
    s5l_nor_program(&m.nor, 0x40u, &flashed, 4);

    uint32_t got = m.bus.read32(m.bus.ctx, S5L8900_NOR_BASE + 0x40u);
    CHECK(got == flashed,
          "NOR read at 0x%08x = %08x, expect %08x — it is being shadowed",
          S5L8900_NOR_BASE + 0x40u, got, flashed);
    CHECK(got != ramdisk, "the NOR read returned the RAM pattern");
    CHECK(m.unmapped_reads == 0,
          "the NOR window must be mapped, not counted unmapped (%llu)",
          (unsigned long long)m.unmapped_reads);

    /* The flash-only property. RAM cannot pass this. */
    m.bus.write32(m.bus.ctx, S5L8900_NOR_BASE + 0x40u, 0xffffffffu);
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_NOR_BASE + 0x40u) == flashed,
          "a 1-setting write was accepted: this window is RAM, not flash");

    /* ...and clearing bits, the direction real programming goes, must work
     * through the bus — this is the path a persisted payload takes. */
    m.bus.write32(m.bus.ctx, S5L8900_NOR_BASE + 0x40u, 0x03030303u);
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_NOR_BASE + 0x40u) == 0x03030303u,
          "programming through the bus did not take");

    printf("  [-R 512] DRAM %08x..%08x, NOR@%08x reads %08x (RAM held %08x)\n",
           base, base + size, S5L8900_NOR_BASE + 0x40u,
           m.bus.read32(m.bus.ctx, S5L8900_NOR_BASE + 0x40u), ramdisk);
    s5l8900_free(&m);
}

/*
 * The general guard, stated as the invariant rather than as one special case:
 * a machine that CONSTRUCTS never has RAM sitting on top of anything it
 * decodes. Probing every window's own edge is what makes this general — add a
 * device tomorrow and it is covered without touching this test.
 */
static void test_no_window_the_machine_decodes_is_shadowed_by_ram(void) {
    s5l8900_t base_m;
    CHECK(s5l8900_init(&base_m, 0, 1u << 20), "machine init failed");
    s5l_window_t w[S5L_WINDOW_MAX];
    unsigned nw = s5l8900_windows(&base_m, w, S5L_WINDOW_MAX);
    CHECK(nw > 0 && nw <= S5L_WINDOW_MAX,
          "window count %u is outside S5L_WINDOW_MAX", nw);
    s5l8900_free(&base_m);

    for (unsigned i = 0; i < nw && i < S5L_WINDOW_MAX; i++) {
        /* A 8 KiB aperture straddling this window's first byte. Deliberately
         * tiny: if the guard were broken the test must fail, not allocate a
         * gigabyte. */
        uint32_t rb = w[i].base - 0x1000u, rs = 0x2000u;
        s5l8900_t t;
        if (!s5l8900_init(&t, rb, rs)) {
            /* Refused. That is a correct outcome, and it must be explicable. */
            CHECK(s5l8900_ram_conflict(rb, rs) != NULL,
                  "init refused RAM 0x%08x+0x%x but names no conflict", rb, rs);
            continue;
        }
        /* Constructed. Then nothing it decodes may be under RAM — including
         * the window we aimed at, which must therefore have been dropped. */
        s5l_window_t got[S5L_WINDOW_MAX];
        unsigned ng = s5l8900_windows(&t, got, S5L_WINDOW_MAX);
        for (unsigned k = 0; k < ng && k < S5L_WINDOW_MAX; k++)
            CHECK(!s5l8900_overlaps(got[k].base, got[k].size, t.ram_base, t.ram_size),
                  "%s 0x%08x+0x%x is under RAM 0x%08x+0x%x",
                  got[k].name, got[k].base, got[k].size, t.ram_base, t.ram_size);
        /* A window that had to be dropped is never dropped silently. */
        CHECK(ng == nw || t.stub_declare_failures > 0,
              "a window vanished (%u -> %u) without being counted", nw, ng);
        s5l8900_free(&t);
    }

    /* Adjacency is not overlap: RAM ending exactly at a window's base is fine,
     * and is precisely the -R 512 case. */
    s5l8900_t ok;
    CHECK(s5l8900_init(&ok, S5L8900_NOR_BASE - 0x1000u, 0x1000u),
          "RAM ending exactly at the NOR base must be allowed");
    CHECK(s5l8900_ram_conflict(S5L8900_NOR_BASE - 0x1000u, 0x1000u) == NULL,
          "abutting RAM must not be reported as a conflict");
    s5l8900_free(&ok);

    /* And one byte more must not be. */
    CHECK(s5l8900_ram_conflict(S5L8900_NOR_BASE - 0x1000u, 0x1001u) != NULL,
          "RAM overlapping the NOR by one byte must be a conflict");
    s5l8900_t bad;
    CHECK(!s5l8900_init(&bad, S5L8900_SDRAM_BASE, 0x20100000u),
          "a DRAM window that reaches into the NOR must be refused, not aliased");
}

/*
 * Where the NOR window is allowed to be. Both bounds are evidence, not taste:
 *
 *   - it must sit above the largest DRAM the guest kernel can use. xnu-1357's
 *     arm_vm_init fixes virtual_avail at 0xe0000000, so with gVirtBase
 *     0xc0000000 mem_size tops out at 512 MB and DRAM cannot pass
 *     0x08000000 + 512 MB = 0x28000000;
 *   - it must sit below arm-io at 0x38000000, which /arm-io's ranges claim
 *     wholesale, and outside 0x18000000..0x28000000, the SoC's other claimed
 *     window (edram, vrom, sram/amc live in it).
 *
 * 0x24000000 satisfied neither: it was inside the DRAM we boot with, and inside
 * a range the device tree assigns to the SoC.
 */
static void test_the_nor_window_is_out_of_every_drams_reach(void) {
    const uint32_t max_dram_end = S5L8900_SDRAM_BASE + (512u << 20);
    CHECK(S5L8900_NOR_BASE >= max_dram_end,
          "NOR base 0x%08x is inside the largest usable DRAM (ends 0x%08x)",
          S5L8900_NOR_BASE, max_dram_end);
    CHECK((uint64_t)S5L8900_NOR_BASE + S5L8900_NOR_SIZE <= S5L8900_ARMIO_BASE,
          "the NOR window runs into arm-io at 0x%08x", S5L8900_ARMIO_BASE);
    CHECK(s5l8900_ram_conflict(S5L8900_SDRAM_BASE, 512u << 20) == NULL,
          "the -R 512 DRAM window must conflict with nothing");
    /* The value it used to have, pinned as a regression rather than a memory. */
    CHECK(s5l8900_ram_conflict(S5L8900_SDRAM_BASE, 512u << 20) == NULL &&
          0x24000000u < max_dram_end,
          "0x24000000 was inside the -R 512 DRAM window; that is why it moved");
}

/*
 * The SoC's own regions, as the shipped device tree gives them. This is the map
 * the routing contract is argued from, so it is worth pinning: /arm-io ranges
 * are (child, parent, size) {0,0x38000000,0x08000000} and
 * {0x10000000,0x18000000,0x10000000}, and the second is anchored twice — vrom's
 * reg 0x18000000 lands on 0x20000000 (the known S5L8900 boot-ROM address) and
 * amc's second reg 0x1a000000 lands on 0x22000000, which is also the link
 * address carried inside firmware/llb.bin.
 */
static void test_soc_regions_match_the_device_tree(void) {
    const s5l_window_t *r = NULL;
    unsigned n = s5l8900_soc_regions(&r);
    CHECK(n > 0 && r != NULL, "the SoC region table must not be empty");

    CHECK(S5L8900_ARMIO_BASE == 0x38000000u && S5L8900_ARMIO_SIZE == 0x08000000u,
          "arm-io = ranges triple 1's parent+size");
    CHECK(S5L8900_VROM_BASE == 0x20000000u,
          "vrom reg 0x18000000 over ranges triple 2 (+0x08000000) = 0x20000000");
    CHECK(S5L8900_SRAM_BASE == 0x22000000u && S5L8900_SRAM_SIZE == 0x0002c000u,
          "amc reg[1] {0x1a000000,0x2c000} over the same range = 0x22000000");
    CHECK(S5L8900_EDRAM_BASE == 0x18000000u,
          "edram reg 0x10000000 over the same range = 0x18000000");
    CHECK(S5L8900_SDRAM_BASE == 0x08000000u, "DRAM base");

    /* No region may overlap another, or the derivation is wrong somewhere. */
    for (unsigned i = 0; i < n; i++)
        for (unsigned k = i + 1; k < n; k++)
            CHECK(!s5l8900_overlaps(r[i].base, r[i].size, r[k].base, r[k].size),
                  "SoC regions %s and %s overlap", r[i].name, r[k].name);

    /*
     * Said out loud, because it is the honest limit of what we claim: the SoC
     * decodes edram at 0x18000000, so no DRAM above 256 MB can be physically
     * real on this part. We allow bigger anyway — a RAM-disk root does not fit
     * otherwise — and an oversized window therefore covers regions the hardware
     * has. That is not a silent alias: we do not model those regions, so we
     * decode nothing there, and the moment one of them becomes a device model
     * it joins DEVICE_WINDOWS and an oversized -R stops constructing.
     */
    unsigned covered = 0;
    for (unsigned i = 0; i < n; i++)
        if (r[i].base > S5L8900_SDRAM_BASE &&
            s5l8900_overlaps(S5L8900_SDRAM_BASE, 512u << 20, r[i].base, r[i].size))
            covered++;
    CHECK(covered == 3,
          "expected the -R 512 window to cover edram, vrom and sram/amc, got %u",
          covered);
    printf("  [map] DRAM 0x%08x, edram 0x%08x, vrom 0x%08x, sram 0x%08x, "
           "arm-io 0x%08x, NOR (ours) 0x%08x\n",
           S5L8900_SDRAM_BASE, S5L8900_EDRAM_BASE, S5L8900_VROM_BASE,
           S5L8900_SRAM_BASE, S5L8900_ARMIO_BASE, S5L8900_NOR_BASE);
}

/*
 * S5L8900_GPIO_BASE was 0x3cf00000 — the S5L8720-era address. The device tree
 * says /arm-io/gpio is reg {0x6400000,0x1000} over ranges child+0x38000000,
 * and the guest's page tables agree. It was unused, so it was a landmine
 * rather than a live bug; this pins it so it cannot drift back.
 */
static void test_gpio_base_is_the_s5l8900_address(void) {
    CHECK(S5L8900_GPIO_BASE == 0x3e400000u,
          "S5L8900_GPIO_BASE=0x%08x expect 0x3e400000 (device tree + page walk)",
          S5L8900_GPIO_BASE);
}

/*
 * There are two PL192 VICs (device tree: reg {0xe00000,0x2000}, vic-stride
 * 0x1000) and AppleARMPL192VIC maps both pages. VIC1 used to be unmapped, so
 * its registers read back as zero and its outputs went nowhere — meaning any
 * device on a line above 31 (the watchdog at 0x33, SDIO at 0x2a, GPIO at
 * 0x20/0x21) could be correctly programmed and still never reach the CPU.
 */
static void test_vic1_is_mapped_and_drives_the_cpu(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    /* It is a real controller, not a hole: enables read back. */
    m.bus.write32(m.bus.ctx, S5L8900_VIC1_BASE + VIC_INTENABLE, 1u << 3);
    CHECK(m.bus.read32(m.bus.ctx, S5L8900_VIC1_BASE + VIC_INTENABLE) == (1u << 3),
          "VIC1 INTENABLE=%08x expect bit 3",
          m.bus.read32(m.bus.ctx, S5L8900_VIC1_BASE + VIC_INTENABLE));
    CHECK(m.unmapped_reads == 0 && m.unmapped_writes == 0,
          "VIC1 accesses must not be unmapped");

    /* And it reaches the CPU. Assert a line on VIC1 only and tick. */
    s5l_vic_set_line(&m.vic[1], 3, true);
    s5l8900_tick(&m, 1);
    CHECK(m.cpu.irq_line,
          "an enabled VIC1 line must raise the CPU's IRQ — otherwise every "
          "device-tree interrupt above 31 is unreachable");

    /* Routing still works independently on each controller. */
    m.bus.write32(m.bus.ctx, S5L8900_VIC1_BASE + VIC_INTSELECT, 1u << 3);
    s5l8900_tick(&m, 1);
    CHECK(!m.cpu.irq_line && m.cpu.fiq_line, "VIC1 select must route to FIQ");

    s5l8900_free(&m);
}

/*
 * The CLCD's reason to be a device model rather than a stub.
 *
 * AppleH1CLCD submits a framebuffer swap, sets bit 0 of the interrupt mask at
 * 0x014, and then waits for bit 0 of the status at 0x018. Storage that returns
 * what was last written returns zero forever and the swap never completes — the
 * UI wedges with no error at all. So the frame interrupt has to arrive on its
 * own, at about the panel's refresh rate in GUEST time.
 */
static void test_clcd_raises_the_frame_interrupt(void) {
    s5l_clcd_t c; s5l_clcd_reset(&c);

    /* Nothing before scanout starts, however long we wait. */
    s5l_clcd_write(&c, CLCD_INTMASK, CLCD_INT_FRAME);
    CHECK(!s5l_clcd_tick(&c, c.frame_ticks * 10u),
          "no frame interrupt before CLCD_ENABLE is written");
    CHECK(c.frames == 0, "frames=%llu expect 0 while stopped",
          (unsigned long long)c.frames);

    /* The driver's own order: mask first, enable last. */
    s5l_clcd_write(&c, CLCD_ENABLE, 1);
    CHECK(!s5l_clcd_tick(&c, c.frame_ticks - 1u),
          "the first VBL must be a whole frame after start, not immediate");
    CHECK(s5l_clcd_tick(&c, 1), "a frame's worth of ticks must raise the line");
    CHECK((s5l_clcd_read(&c, CLCD_INTSTATUS) & CLCD_INT_FRAME) != 0,
          "status=%08x expect bit 0 set at VBL",
          s5l_clcd_read(&c, CLCD_INTSTATUS));

    /* Never the underrun bits: they only make a correct driver log errors. */
    CHECK((s5l_clcd_read(&c, CLCD_INTSTATUS) & CLCD_INT_UNDERRUN) == 0,
          "the underrun bits must never be asserted");

    /* Write-1-to-clear, exactly as the handler acknowledges at 0xc0705dac. */
    s5l_clcd_write(&c, CLCD_INTSTATUS, CLCD_INT_FRAME);
    CHECK(s5l_clcd_read(&c, CLCD_INTSTATUS) == 0,
          "status=%08x expect cleared by the driver's write-1 acknowledge",
          s5l_clcd_read(&c, CLCD_INTSTATUS));
    CHECK(!s5l_clcd_tick(&c, 0), "the line must drop once acknowledged");

    /* Steady, one per frame — not a burst and not a stall. */
    unsigned n = 0;
    for (unsigned i = 0; i < 10u * c.frame_ticks; i++)
        if (s5l_clcd_tick(&c, 1)) { n++; s5l_clcd_write(&c, CLCD_INTSTATUS, CLCD_INT_FRAME); }
    CHECK(n == 10, "raised %u frames in 10 frames' worth of ticks", n);
}

/* A host scheduler can hand the machine a very large time slice. The phase
 * arithmetic must not wrap at 32 bits and silently postpone VBL forever. */
static void test_clcd_large_tick_preserves_phase(void) {
    s5l_clcd_t c; s5l_clcd_reset(&c);
    c.frame_ticks = 100u;
    c.scanning = true;
    c.frame_accum = 99u;
    c.intmask = CLCD_INT_FRAME;

    const uint64_t total = UINT64_C(99) + UINT32_MAX;
    CHECK(s5l_clcd_tick(&c, UINT32_MAX),
          "a huge tick crossing a frame must raise the frame line");
    CHECK(c.frame_accum == (uint32_t)(total % c.frame_ticks),
          "phase=%u expect %u after a UINT32_MAX tick",
          c.frame_accum, (uint32_t)(total % c.frame_ticks));
    CHECK(c.frames == total / c.frame_ticks,
          "frames=%llu expect %llu after a UINT32_MAX tick",
          (unsigned long long)c.frames,
          (unsigned long long)(total / c.frame_ticks));
}

/*
 * The interrupt LINE must be gated by the hardware mask at 0x014, not by the
 * status alone. The handler at 0xc0705d7c computes `status & shadowMask` and
 * only acknowledges what is in that intersection. If the line were asserted for
 * a source the mask has turned off, the handler would clear nothing, return,
 * and be re-entered immediately — an interrupt storm, which is the same failure
 * that the timer's acknowledge mask exists to avoid.
 */
static void test_clcd_line_is_gated_by_the_mask(void) {
    s5l_clcd_t c; s5l_clcd_reset(&c);
    s5l_clcd_write(&c, CLCD_ENABLE, 1);

    /* Mask clear: a frame still latches in the status (the hardware does not
     * stop counting) but nothing reaches the controller. */
    CHECK(!s5l_clcd_tick(&c, c.frame_ticks), "masked-off frame must not raise the line");
    CHECK((s5l_clcd_read(&c, CLCD_INTSTATUS) & CLCD_INT_FRAME) != 0,
          "the status latch should still record the frame");

    /* This is the driver's actual swap_submit sequence at 0xc0705d44: clear the
     * stale status FIRST, then enable. If the status were not clearable the
     * very first swap would look already-complete. */
    s5l_clcd_write(&c, CLCD_INTSTATUS, CLCD_INT_FRAME);
    s5l_clcd_write(&c, CLCD_INTMASK, 0x3f00u | CLCD_INT_FRAME);
    CHECK(!s5l_clcd_tick(&c, 0),
          "arming the mask must not immediately re-raise a cleared status");
    CHECK(s5l_clcd_tick(&c, c.frame_ticks), "the next frame must raise the line");

    /* And the underrun bits are enabled in that same mask (0x3f01), so if we
     * ever set them the driver would log an error every frame. */
    CHECK((s5l_clcd_read(&c, CLCD_INTSTATUS) & CLCD_INT_UNDERRUN) == 0,
          "underrun must stay clear even while it is unmasked");
}

/*
 * 0x204 must not read as 0xC0. The driver does
 *   0xc0705ccc  ldr r3,[reg,#0x204]; lsr r3,r3,#6; and r3,r3,#3; cmp r3,#3
 * and DEFERS the swap when both bits are set. A stub that echoed a previous
 * write, or any model that guessed at those bits, could stall every swap the
 * display ever attempts. Zero is "not busy" and is the only answer that cannot
 * deadlock.
 */
static void test_clcd_status_never_defers_the_swap(void) {
    s5l_clcd_t c; s5l_clcd_reset(&c);
    CHECK(((s5l_clcd_read(&c, CLCD_STATUS) >> 6) & 3u) != 3u,
          "status=%08x has bits[7:6] set — AppleH1CLCD would defer every swap",
          s5l_clcd_read(&c, CLCD_STATUS));
    /* Not even if the guest writes it: it is read-only. */
    s5l_clcd_write(&c, CLCD_STATUS, 0xffffffffu);
    CHECK(s5l_clcd_read(&c, CLCD_STATUS) == 0,
          "status=%08x expect 0 — a write must not be able to stall the display",
          s5l_clcd_read(&c, CLCD_STATUS));
}

/*
 * Everything else on the page is storage the driver saves and restores across
 * sleep. It never initialises the panel timing at 0x0d8..0x0ec — it only saves
 * and restores it — so iBoot must set it, and read-back is the whole contract.
 */
static void test_clcd_saved_registers_read_back(void) {
    s5l_clcd_t c; s5l_clcd_reset(&c);
    static const uint32_t OFFS[] = {
        CLCD_TIMING0, CLCD_TIMING0 + 4u, CLCD_TIMING0 + 8u, CLCD_TIMING0 + 12u,
        CLCD_TIMING4, CLCD_CTRL, CLCD_FIFO, CLCD_BACKDROP,
        CLCD_VIDEO_FIRST, CLCD_VIDEO_LAST, CLCD_CSC_FIRST, CLCD_CSC_LAST,
        CLCD_OPAQUE_FIRST, CLCD_OPAQUE_LAST, CLCD_GATE,
        CLCD_GAMMA0, CLCD_GAMMA0 + CLCD_GAMMA_SIZE - 4u,
        CLCD_GAMMA0 + CLCD_GAMMA_SIZE, CLCD_GAMMA0 + 3u * CLCD_GAMMA_SIZE - 4u,
    };
    for (unsigned i = 0; i < sizeof OFFS / sizeof OFFS[0]; i++) {
        s5l_clcd_write(&c, OFFS[i], 0xa5000000u + i);
        CHECK(s5l_clcd_read(&c, OFFS[i]) == 0xa5000000u + i,
              "offset 0x%03x read=%08x expect %08x", OFFS[i],
              s5l_clcd_read(&c, OFFS[i]), 0xa5000000u + i);
    }
    /* The three LUTs must be distinct, not aliases of one another. */
    CHECK(s5l_clcd_read(&c, CLCD_GAMMA0) !=
          s5l_clcd_read(&c, CLCD_GAMMA0 + CLCD_GAMMA_SIZE),
          "the gamma LUTs must not alias each other");
}

/*
 * Seeding stands in for iBoot: window 0 programmed over a running scanout, so
 * IOMobileFramebuffer::start adopts our framebuffer instead of inventing one.
 * The property that matters is that what iBoot wrote is what the guest reads —
 * through the real register path, at the real offsets.
 */
static void test_clcd_seed_is_visible_to_the_guest(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    const uint32_t FB = 0x0ff00000u, W = 320, H = 480, STRIDE = 320 * 4;
    CHECK(s5l_clcd_seed_window0(&m.clcd, FB, W, H, STRIDE,
                                CLCD_FMT_32BPP, CLCD_ORDER_BGRA),
          "the iPhone 3G's 320x480 framebuffer must be representable");

    uint32_t b = S5L8900_CLCD_BASE + CLCD_WIN_FIRST;
    CHECK(m.bus.read32(m.bus.ctx, b + CLCD_WIN_FBADDR) == FB,
          "window 0 base=%08x expect %08x",
          m.bus.read32(m.bus.ctx, b + CLCD_WIN_FBADDR), FB);
    CHECK(m.bus.read32(m.bus.ctx, b + CLCD_WIN_GEOMETRY) == ((W << 16) | H),
          "window 0 geometry=%08x expect %08x",
          m.bus.read32(m.bus.ctx, b + CLCD_WIN_GEOMETRY), (W << 16) | H);
    CHECK(m.bus.read32(m.bus.ctx, b + CLCD_WIN_PITCH) == STRIDE, "stride");
    CHECK(m.bus.read32(m.bus.ctx, b + CLCD_WIN_LINEWORDS) == STRIDE / 4u, "line words");
    CHECK(((m.bus.read32(m.bus.ctx, b + CLCD_WIN_CONTROL) >> CLCD_FMT_SHIFT)
           & CLCD_FMT_MASK) == CLCD_FMT_32BPP, "pixel format");
    CHECK((m.bus.read32(m.bus.ctx, S5L8900_CLCD_BASE + CLCD_CTRL) & CLCD_CTRL_WIN0) != 0,
          "window 0 must be enabled in CLCD_CTRL");

    /* And the host-side accessor reports the same thing, which is the seam a
     * renderer reads. */
    uint32_t fb = 0, w = 0, h = 0, st = 0, fmt = 0, ord = 0;
    CHECK(s5l_clcd_window(&m.clcd, 0, &fb, &w, &h, &st, &fmt, &ord),
          "window 0 should report as enabled");
    CHECK(fb == FB && w == W && h == H && st == STRIDE &&
          fmt == CLCD_FMT_32BPP && ord == CLCD_ORDER_BGRA,
          "window 0 = {%08x %ux%u stride %u fmt %u order %u}", fb, w, h, st, fmt, ord);
    CHECK(!s5l_clcd_window(&m.clcd, 1, &fb, &w, &h, &st, &fmt, &ord),
          "window 1 is not enabled and must not report as if it were");

    /* Seeding must not be able to fire an interrupt at a guest that has not
     * asked for one: the mask is still zero. */
    s5l8900_tick(&m, m.cpu_hz);                 /* a whole guest second */
    CHECK(!m.cpu.irq_line, "a seeded display must not interrupt before the "
                           "guest enables the frame interrupt");
    CHECK(m.clcd.frames > 0, "scanout should have been running all the same");

    s5l8900_free(&m);
}

/* The host-side iBoot shim is a trust boundary: malformed or overflowed seed
 * data must be rejected atomically, never masked into plausible registers. */
static void test_clcd_seed_rejects_invalid_layouts_atomically(void) {
    s5l_clcd_t c; s5l_clcd_reset(&c);
    CHECK(s5l_clcd_seed_window0(&c, 0x0ff00000u, 320u, 480u, 1280u,
                                CLCD_FMT_32BPP, CLCD_ORDER_BGRA),
          "baseline seed failed");
    s5l_clcd_t before = c;

    struct {
        uint32_t fb, width, height, stride, format, order;
        const char *why;
    } bad[] = {
        { 0x0ff00000u, 0u,      480u, 1280u, CLCD_FMT_32BPP, CLCD_ORDER_BGRA,
          "zero width" },
        { 0x0ff00000u, 0x800u,  480u, 8192u, CLCD_FMT_32BPP, CLCD_ORDER_BGRA,
          "width field overflow" },
        { 0x0ff00000u, 320u,      0u, 1280u, CLCD_FMT_32BPP, CLCD_ORDER_BGRA,
          "zero height" },
        { 0x0ff00000u, 320u,    0x400u, 1280u, CLCD_FMT_32BPP, CLCD_ORDER_BGRA,
          "height field overflow" },
        { 0x0ff00000u, 320u,    480u, 1276u, CLCD_FMT_32BPP, CLCD_ORDER_BGRA,
          "stride shorter than a row" },
        { 0x0ff00000u, 320u,    480u, 1282u, CLCD_FMT_32BPP, CLCD_ORDER_BGRA,
          "stride not word aligned" },
        { 0x0ff00000u, 320u,    480u, 1280u, CLCD_FMT_MASK + 1u, CLCD_ORDER_BGRA,
          "format field overflow" },
        { 0x0ff00000u, 320u,    480u, 1280u, CLCD_FMT_32BPP, CLCD_ORDER_MASK + 1u,
          "order field overflow" },
        { 0xfffffff1u, 4u,        1u,   16u, CLCD_FMT_32BPP, CLCD_ORDER_BGRA,
          "physical address wrap" },
    };

    CHECK(!s5l_clcd_seed_window0(NULL, 0, 1, 1, 4,
                                 CLCD_FMT_32BPP, CLCD_ORDER_BGRA),
          "a null controller must be rejected");
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(!s5l_clcd_seed_window0(&c, bad[i].fb, bad[i].width,
                                     bad[i].height, bad[i].stride,
                                     bad[i].format, bad[i].order),
              "accepted %s", bad[i].why);
        CHECK(memcmp(&c, &before, sizeof c) == 0,
              "%s changed controller state on rejection", bad[i].why);
    }

    s5l_clcd_t edge; s5l_clcd_reset(&edge);
    CHECK(s5l_clcd_seed_window0(&edge, 0xfffffff0u, 4u, 1u, 16u,
                                CLCD_FMT_32BPP, CLCD_ORDER_BGRA),
          "a framebuffer ending exactly at 4 GiB was rejected");
}

/*
 * The window the DRIVER would pick, in the driver's own order. AppleH1CLCD
 * tests 0x40, then 0x20, then 0x10, then 0x08 (0xc0705f10..0xc0705f94) and
 * scans out the FIRST match; it does not merge windows and it does not prefer
 * the largest. Getting the order wrong here would put the picture in the wrong
 * place with no error anywhere.
 */
static void test_clcd_active_window_follows_the_drivers_order(void) {
    s5l_clcd_t c; s5l_clcd_reset(&c);
    CHECK(s5l_clcd_active_window(&c) == CLCD_WIN_NONE,
          "a controller with no window enabled must say so, not name window 0");

    s5l_clcd_write(&c, CLCD_CTRL, CLCD_CTRL_WIN2 | CLCD_CTRL_WIN3);
    CHECK(s5l_clcd_active_window(&c) == 2,
          "with windows 2 and 3 enabled the driver takes 2, got %u",
          s5l_clcd_active_window(&c));

    s5l_clcd_write(&c, CLCD_CTRL, CLCD_CTRL_WIN1 | CLCD_CTRL_WIN2 | CLCD_CTRL_WIN3);
    CHECK(s5l_clcd_active_window(&c) == 1, "window 1 outranks 2 and 3");

    s5l_clcd_write(&c, CLCD_CTRL, 0xffu);
    CHECK(s5l_clcd_active_window(&c) == 0, "window 0 outranks everything");

    /* CLCD_CTRL_VIDEO is not an RGB window and must not be mistaken for one. */
    s5l_clcd_write(&c, CLCD_CTRL, CLCD_CTRL_VIDEO | CLCD_CTRL_ENABLE);
    CHECK(s5l_clcd_active_window(&c) == CLCD_WIN_NONE,
          "the video overlay bit is not an RGB window");
}

/*
 * Scanout: the pixels the guest wrote are the pixels that come out, and a
 * window that is programmed wrong produces an ERROR rather than a picture.
 */
static void test_clcd_scanout_reads_guest_memory(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0x08000000u, 1u << 20), "machine init failed");

    const uint32_t W = 4, H = 3, STRIDE = 64;       /* stride > W*bpp on purpose */
    const uint32_t FB = 0x08000000u + 0x1000u;
    s5l_clcd_seed_window0(&m.clcd, FB, W, H, STRIDE,
                          CLCD_FMT_32BPP, CLCD_ORDER_BGRA);

    /* Paint a known pattern through the bus, as the guest would. Stored as
     * 0xAARRGGBB, so byte 0 is blue and byte 2 is red. */
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++)
            m.bus.write32(m.bus.ctx, FB + y * STRIDE + x * 4u,
                          0xff000000u | ((x * 16u) << 16) | ((y * 32u) << 8) | 7u);

    uint8_t rgb[4 * 3 * 3];
    uint32_t ow = 0, oh = 0;
    CHECK(s5l_clcd_scanout(&m.clcd, 0, m.ram, m.ram_base, m.ram_size,
                           rgb, sizeof rgb, &ow, &oh),
          "scanout of a correctly programmed window must succeed");
    CHECK(ow == W && oh == H, "scanout geometry %ux%u, expected %ux%u", ow, oh, W, H);
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            const uint8_t *p = &rgb[(y * W + x) * 3];
            CHECK(p[0] == (uint8_t)(x * 16u) && p[1] == (uint8_t)(y * 32u) && p[2] == 7,
                  "pixel (%u,%u) = %02x%02x%02x", x, y, p[0], p[1], p[2]);
        }

    /* A buffer one byte short must be refused, not overrun. */
    CHECK(!s5l_clcd_scanout(&m.clcd, 0, m.ram, m.ram_base, m.ram_size,
                            rgb, sizeof rgb - 1u, NULL, NULL),
          "a short destination must be an error");

    /* A window whose last row runs past the end of DRAM must be refused. Placing
     * the base so that h-1 strides plus one row overshoots by a single byte is
     * the case an off-by-one would wave through. */
    uint32_t last = (uint32_t)(m.ram_base + m.ram_size);
    uint32_t need = (H - 1u) * STRIDE + W * 4u;
    s5l_clcd_seed_window0(&m.clcd, last - need + 1u, W, H, STRIDE,
                          CLCD_FMT_32BPP, CLCD_ORDER_BGRA);
    CHECK(!s5l_clcd_scanout(&m.clcd, 0, m.ram, m.ram_base, m.ram_size,
                            rgb, sizeof rgb, NULL, NULL),
          "a window running one byte past DRAM must be an error, not a picture");

    /* A stride too small to hold a row is a programming fault, not a squeeze. */
    /* Guest MMIO can still produce malformed state even though the host seed
     * API rejects it, so exercise that path through the real register write. */
    s5l_clcd_write(&m.clcd, CLCD_WIN_FIRST + CLCD_WIN_PITCH, W * 4u - 1u);
    CHECK(!s5l_clcd_scanout(&m.clcd, 0, m.ram, m.ram_base, m.ram_size,
                            rgb, sizeof rgb, NULL, NULL),
          "a stride shorter than one row must be an error");

    /* A disabled window has nothing to scan out. */
    s5l_clcd_seed_window0(&m.clcd, FB, W, H, STRIDE,
                          CLCD_FMT_32BPP, CLCD_ORDER_BGRA);
    s5l_clcd_write(&m.clcd, CLCD_CTRL, 0);
    CHECK(!s5l_clcd_scanout(&m.clcd, 0, m.ram, m.ram_base, m.ram_size,
                            rgb, sizeof rgb, NULL, NULL),
          "a disabled window must not scan out");

    s5l8900_free(&m);
}

/*
 * 16-bit windows. The driver publishes '565L' for every format below 6, so a
 * 16-bit window is 5-6-5 — and full-scale in has to be full-scale out, which a
 * plain left-shift does not give you (0x1f << 3 is 0xf8, not 0xff).
 */
static void test_clcd_scanout_565_reaches_full_white(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0x08000000u, 1u << 20), "machine init failed");

    const uint32_t W = 2, H = 1, STRIDE = 8, FB = 0x08000000u + 0x2000u;
    s5l_clcd_seed_window0(&m.clcd, FB, W, H, STRIDE,
                          CLCD_FMT_RGB565, CLCD_ORDER_BGRA);
    /* pixel 0 = white (all bits set), pixel 1 = pure green at full scale. */
    m.bus.write32(m.bus.ctx, FB, 0x07e0ffffu);

    uint8_t rgb[2 * 3];
    CHECK(s5l_clcd_scanout(&m.clcd, 0, m.ram, m.ram_base, m.ram_size,
                           rgb, sizeof rgb, NULL, NULL), "565 scanout");
    CHECK(rgb[0] == 0xff && rgb[1] == 0xff && rgb[2] == 0xff,
          "0xffff must decode to full white, got %02x%02x%02x",
          rgb[0], rgb[1], rgb[2]);
    CHECK(rgb[3] == 0 && rgb[4] == 0xff && rgb[5] == 0,
          "0x07e0 must decode to full green, got %02x%02x%02x",
          rgb[3], rgb[4], rgb[5]);

    s5l8900_free(&m);
}

/*
 * The whole path, end to end: the guest arms the frame interrupt exactly the
 * way AppleH1CLCD's swap_submit does, and the CPU's IRQ line comes up through
 * VIC line 13 — the line the device tree gives /arm-io/clcd.
 */
static void test_clcd_interrupt_reaches_the_cpu_on_line_13(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << S5L8900_IRQ_CLCD);
    /* swap_submit: clear stale status, then enable the frame interrupt. */
    m.bus.write32(m.bus.ctx, S5L8900_CLCD_BASE + CLCD_INTSTATUS, CLCD_INT_FRAME);
    m.bus.write32(m.bus.ctx, S5L8900_CLCD_BASE + CLCD_INTMASK,
                  CLCD_INT_UNDERRUN | CLCD_INT_FRAME);
    m.bus.write32(m.bus.ctx, S5L8900_CLCD_BASE + CLCD_ENABLE, 1);

    /* One guest frame of instructions at the real CPU:timebase ratio. */
    uint32_t insns = (uint32_t)((uint64_t)m.cpu_hz / S5L_CLCD_REFRESH_HZ) + 16u;
    for (uint32_t i = 0; i < insns; i++) s5l8900_tick(&m, 1);

    CHECK(m.cpu.irq_line,
          "no IRQ after a guest frame — swap_submit would never complete");
    uint32_t st = m.bus.read32(m.bus.ctx, S5L8900_CLCD_BASE + CLCD_INTSTATUS);
    CHECK((st & CLCD_INT_FRAME) != 0, "status=%08x expect the frame bit", st);
    CHECK((st & CLCD_INT_UNDERRUN) == 0, "status=%08x must never report underrun", st);
    CHECK((m.bus.read32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_IRQSTATUS) &
           (1u << S5L8900_IRQ_CLCD)) != 0,
          "the CLCD must present on VIC line 13, as the device tree says");

    /* Acknowledge as the handler does, and the line must drop. */
    m.bus.write32(m.bus.ctx, S5L8900_CLCD_BASE + CLCD_INTSTATUS, CLCD_INT_FRAME);
    s5l8900_tick(&m, 1);
    CHECK(!m.cpu.irq_line, "the IRQ must drop once the frame bit is acknowledged");

    printf("  [CLCD] %llu frames in one guest frame period, VIC line %u\n",
           (unsigned long long)m.clcd.frames, S5L8900_IRQ_CLCD);
    s5l8900_free(&m);
}

/*
 * VICADDRESS (0xF00) is how AppleARMPL192VIC finds the pending source — it does
 * NOT read IRQSTATUS. It decodes the returned word as source|0x80000000 and
 * dispatches. Returning a bare 0 (no valid tag) decodes to spurious source 0,
 * so a real interrupt is acknowledged without its handler running and re-fires
 * forever — the boot-stopping storm the self-IPI on line 4 produced.
 */
static void test_vic_vectaddr_reports_tagged_source(void) {
    s5l_vic_t v; s5l_vic_reset(&v);

    /* Nothing pending -> 0, which the driver correctly reads as spurious. */
    CHECK(s5l_vic_vectaddr(&v, 0) == 0, "idle VICADDRESS must be 0");

    /* Reproduce the storm exactly: software interrupt bit 4, enabled, routed to
     * IRQ (not selected to FIQ) — the self-IPI. */
    s5l_vic_write(&v, VIC_SOFTINT, 1u << 4);
    s5l_vic_write(&v, VIC_INTENABLE, 1u << 4);
    CHECK(s5l_vic_vectaddr(&v, 0) == (0x80000000u | 4u),
          "VICADDRESS=%08x expect 80000004 (source 4, valid bit) — a bare 0 "
          "here is the storm", s5l_vic_vectaddr(&v, 0));

    /* The driver then clears it via SOFTINTCLEAR, and it must go idle. */
    s5l_vic_write(&v, VIC_SOFTINTCLEAR, 1u << 4);
    CHECK(s5l_vic_vectaddr(&v, 0) == 0,
          "after SOFTINTCLEAR the source must be gone, not re-reported");

    /* base_source places a VIC in the daisy chain: VIC1's line 4 is global 36. */
    s5l_vic_reset(&v);
    s5l_vic_write(&v, VIC_SOFTINT, 1u << 4);
    s5l_vic_write(&v, VIC_INTENABLE, 1u << 4);
    CHECK(s5l_vic_vectaddr(&v, 32) == (0x80000000u | 36u),
          "VIC1 line 4 must report as global source 36");

    /* Lowest-numbered pending line wins (default priority). */
    s5l_vic_reset(&v);
    s5l_vic_write(&v, VIC_INTENABLE, 0xffffffffu);
    s5l_vic_set_line(&v, 9, true);
    s5l_vic_set_line(&v, 3, true);
    CHECK(s5l_vic_vectaddr(&v, 0) == (0x80000000u | 3u),
          "lowest pending line (3) must be selected, got %08x",
          s5l_vic_vectaddr(&v, 0));

    /* A line routed to FIQ must not appear in VICADDRESS (which is IRQ-only). */
    s5l_vic_reset(&v);
    s5l_vic_write(&v, VIC_INTENABLE, 1u << 7);
    s5l_vic_write(&v, VIC_INTSELECT, 1u << 7);
    s5l_vic_set_line(&v, 7, true);
    CHECK(s5l_vic_vectaddr(&v, 0) == 0,
          "an FIQ-routed line must not surface through VICADDRESS");
}

static void test_vic_masks_and_routes(void) {
    s5l_vic_t v; s5l_vic_reset(&v);
    s5l_vic_set_line(&v, 5, true);
    CHECK(!s5l_vic_irq(&v), "line asserted but disabled should not raise IRQ");
    s5l_vic_write(&v, VIC_INTENABLE, 1u << 5);
    CHECK(s5l_vic_irq(&v), "enabled line should raise IRQ");
    s5l_vic_write(&v, VIC_INTSELECT, 1u << 5);
    CHECK(!s5l_vic_irq(&v) && s5l_vic_fiq(&v), "select should route the line to FIQ");
    s5l_vic_write(&v, VIC_INTSELECT, 0);
    s5l_vic_write(&v, VIC_INTENCLEAR, 1u << 5);
    CHECK(!s5l_vic_irq(&v), "cleared enable should drop IRQ");
}

/*
 * The full interrupt path: timer counts down -> VIC masks/routes it -> the CPU
 * takes an IRQ exception -> the guest handler prints 'T' and returns with
 * SUBS pc, lr, #4, landing back in the interrupted spin loop.
 *
 *   0x18: B 0x40          ; IRQ vector
 *   0x40: MOV r1,#'T'
 *   0x44: STR r1,[r0,#0x20]
 *   0x48: MOV r1,#0x00030000
 *   0x4c: STR r1,[r2,#0xf4]  ; acknowledge, exactly as the kernel's handler does
 *   0x50: SUBS pc,lr,#4      ; return from IRQ (restores CPSR from SPSR)
 *   0x100: B .               ; main loop
 */
static void test_timer_interrupt_reaches_handler(void) {
    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    /* B 0x40 from 0x18: target = pc + 8 + off  ->  0x40 = 0x18 + 8 + off */
    const uint32_t branch = 0xea000000u | (((0x40u - 0x18u - 8u) / 4u) & 0x00ffffffu);
    s5l8900_load(&m, 0x18, &branch, 4);

    /* The handler disables the timer so exactly one interrupt fires; that lets
     * us assert precisely where the CPU ends up after returning. */
    const uint32_t handler[] = {
        0xe3a01054u,   /* MOV r1,#0x54 'T'      */
        0xe5801020u,   /* STR r1,[r0,#0x20]     */
        0xe3a01000u,   /* MOV r1,#0             */
        0xe58210a4u,   /* STR r1,[r2,#0xa4]  stop timer 4 */
        0xe3a01803u,   /* MOV r1,#0x00030000    */
        0xe58210f4u,   /* STR r1,[r2,#0xf4]  acknowledge */
        0xe25ef004u    /* SUBS pc,lr,#4      return from IRQ */
    };
    s5l8900_load(&m, 0x40, handler, sizeof handler);

    const uint32_t spin = 0xeafffffeu;              /* B . */
    s5l8900_load(&m, 0x100, &spin, 4);

    /* This test exercises device -> controller -> CPU, not the clock ratio, so
     * run the timebase at one tick per instruction to keep it readable. */
    m.cpu_hz = m.tb_hz = 1;

    /* Program the controller and timer the way guest setup code would. */
    m.bus.write32(m.bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << S5L8900_IRQ_TIMER);
    m.bus.write32(m.bus.ctx, S5L8900_TIMER_BASE + TIMER4_COUNTBUF, 4);
    m.bus.write32(m.bus.ctx, S5L8900_TIMER_BASE + TIMER4_STATE,
                  TIMER4_STATE_START | TIMER4_STATE_UPDATE);

    /* Start in SYS mode with IRQs unmasked, spinning at 0x100. */
    m.cpu.r[15] = 0x100;
    m.cpu.r[0]  = S5L8900_UART0_BASE;
    m.cpu.r[2]  = S5L8900_TIMER_BASE;
    m.cpu.cpsr  = ARM_MODE_SYS;                     /* I and F clear */

    arm_status_t st = ARM_OK;
    s5l8900_run(&m, 200, &st);

    CHECK(st == ARM_OK, "status=%d expect ARM_OK", (int)st);
    m.uart0.tx[m.uart0.tx_len] = '\0';
    CHECK(strcmp(m.uart0.tx, "T") == 0,
          "uart=\"%s\" expect exactly one 'T' (handler ran once)", m.uart0.tx);
    /* Having returned via SUBS pc,lr,#4 we must be back in the interrupted
     * mode, at the interrupted instruction. */
    CHECK((m.cpu.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS,
          "mode=%02x expect back in SYS after IRQ return",
          m.cpu.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(m.cpu.r[15] == 0x100, "pc=%08x expect 100 (resumed the spin loop)",
          m.cpu.r[15]);
    printf("  [timer IRQ -> handler -> return] uart=\"%s\", resumed at pc=%08x\n",
           m.uart0.tx, m.cpu.r[15]);
    s5l8900_free(&m);
}

int main(void) {
    printf("iOS3-VM S5L8900 machine tests\n");
    test_ram_readback();
    test_uart_status_is_ready();
    test_unmapped_access_counted();
    test_bounds_check_cannot_overflow();
    test_bare_metal_uart_hello();
    test_stub_window_stores_and_counts();
    test_mmio_width_alignment_and_window_edges();
    test_address_space_wrap_is_refused();
    test_machine_declares_its_known_windows();
    test_nor_reads_are_nor_at_the_boot_ram_size();
    test_no_window_the_machine_decodes_is_shadowed_by_ram();
    test_the_nor_window_is_out_of_every_drams_reach();
    test_soc_regions_match_the_device_tree();
    test_gpio_base_is_the_s5l8900_address();
    test_power_gate_state_tracks_onctrl_offctrl();
    test_vic_vectaddr_reports_tagged_source();
    test_vic_masks_and_routes();
    test_vic1_is_mapped_and_drives_the_cpu();
    test_clcd_raises_the_frame_interrupt();
    test_clcd_large_tick_preserves_phase();
    test_clcd_line_is_gated_by_the_mask();
    test_clcd_status_never_defers_the_swap();
    test_clcd_saved_registers_read_back();
    test_clcd_seed_is_visible_to_the_guest();
    test_clcd_seed_rejects_invalid_layouts_atomically();
    test_clcd_active_window_follows_the_drivers_order();
    test_clcd_scanout_reads_guest_memory();
    test_clcd_scanout_565_reaches_full_white();
    test_clcd_interrupt_reaches_the_cpu_on_line_13();
    test_timebase_runs_without_a_timer();
    test_timebase_runs_at_the_guest_ratio();
    test_timer_period_is_exact();
    test_timer_ack_mask_matches_the_kernels();
    test_timer_interrupt_reaches_handler();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

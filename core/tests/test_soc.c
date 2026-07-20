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
    test_vic_masks_and_routes();
    test_timebase_runs_without_a_timer();
    test_timer_period_is_exact();
    test_timer_ack_mask_matches_the_kernels();
    test_timer_interrupt_reaches_handler();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

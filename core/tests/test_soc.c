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

int main(void) {
    printf("iOS3-VM S5L8900 machine tests\n");
    test_ram_readback();
    test_uart_status_is_ready();
    test_unmapped_access_counted();
    test_bare_metal_uart_hello();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

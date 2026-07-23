/*
 * iOS3-VM — snapshot / restore tests.
 *
 * Two kinds of test live here and both are necessary.
 *
 *  1. FIELD tests enumerate, by hand, every field of every snapshotted struct
 *     and assert it survives a round trip. This list is deliberately a SECOND,
 *     independent statement of the same fact as snapshot.c's visitors: a field
 *     dropped from a visitor is invisible to any test that only compares one
 *     serialised stream against another, because the dropped field is absent
 *     from both. Only an independent enumeration catches it.
 *
 *  2. The DIVERGENCE test is the unit-scale form of the acceptance test: run
 *     guest code, snapshot mid-flight, continue one machine and restore into
 *     another, run both the same distance, and require the two resulting
 *     snapshots to be byte-identical. That catches a missing field which
 *     affects execution even if nobody thought to list it.
 *
 * Plus the rejection tests: a snapshot that half-loads is worse than one that
 * refuses, so bad magic / bad version / truncation / corruption / a mismatched
 * machine must all be refused, and must leave the target machine untouched.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* Compare one field of two machines. */
#define SAME(path) CHECK(a->path == b->path, #path " did not survive the round trip")

#define RAMSZ (1u << 20)

/* --------------------------------------------------------------- helpers --- */

/* Save `a` to memory, restore it into a freshly initialised `b`. */
static bool roundtrip(const s5l8900_t *a, s5l8900_t *b) {
    uint8_t *buf = NULL; size_t len = 0;
    snapshot_status_t st = snapshot_save_mem(a, &buf, &len);
    if (st != SNAP_OK) { printf("  save failed: %s\n", snapshot_strerror(st)); return false; }
    st = snapshot_load_mem(b, buf, len);
    free(buf);
    if (st != SNAP_OK) { printf("  load failed: %s\n", snapshot_strerror(st)); return false; }
    return true;
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64le(const uint8_t *p) {
    return (uint64_t)rd32le(p) | ((uint64_t)rd32le(p + 4) << 32);
}

static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void wr64le(uint8_t *p, uint64_t v) {
    wr32le(p, (uint32_t)v); wr32le(p + 4, (uint32_t)(v >> 32));
}

static uint64_t snapshot_hash(const uint8_t *p, size_t n) {
    /* Preserve the format's existing (nonstandard) historical FNV offset. */
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static void refresh_snapshot_hash(uint8_t *buf, size_t len) {
    if (!buf || len < 48u) return;
    uint64_t plen = rd64le(buf + 24);
    if (plen <= len - 48u)
        wr64le(buf + 40u + (size_t)plen, snapshot_hash(buf + 40, (size_t)plen));
}

static size_t find_section(const uint8_t *buf, size_t len, uint32_t tag) {
    if (len < 48u) return 0;
    uint64_t plen = rd64le(buf + 24), used = 0;
    size_t off = 40;
    while (used + 16u <= plen && off + 16u <= len) {
        uint64_t slen = rd64le(buf + off + 8);
        if (rd32le(buf + off) == tag) return off;
        if (slen > plen - used - 16u || slen > SIZE_MAX - off - 16u) return 0;
        off += 16u + (size_t)slen;
        used += 16u + slen;
    }
    return 0;
}

/* ------------------------------------------------------------- CPU state --- */

/*
 * Every field of arm_cpu_t, as listed by arm_reset() in arm_interp.c — the
 * authoritative statement of what CPU state is. `bus` is the one member that is
 * intentionally not carried (it is a host pointer); the test asserts instead
 * that a restored CPU is wired to its own machine's bus.
 */
static void test_cpu_state_round_trips(void) {
    s5l8900_t ma, mb;
    s5l8900_t *a = &ma, *b = &mb;
    CHECK(s5l8900_init(a, 0, RAMSZ), "init a");
    CHECK(s5l8900_init(b, 0, RAMSZ), "init b");

    for (unsigned i = 0; i < 16; i++) a->cpu.r[i] = 0x1000u + i * 0x11u;
    a->cpu.cpsr = ARM_MODE_ABT | ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q | ARM_CPSR_T;
    a->cpu.cp15.sctlr      = 0x00c5187du;
    a->cpu.cp15.actlr      = 0x00000007u;
    a->cpu.cp15.cpacr      = 0x00f00000u;
    a->cpu.cp15.ttbr0      = 0x09000000u;
    a->cpu.cp15.ttbr1      = 0x09004000u;
    a->cpu.cp15.ttbcr      = 0x00000002u;
    a->cpu.cp15.dacr       = 0x55555555u;
    a->cpu.cp15.dfsr       = 0x00000805u;
    a->cpu.cp15.ifsr       = 0x00000007u;
    a->cpu.cp15.dfar       = 0xdeadbe00u;
    a->cpu.cp15.ifar       = 0xfeedface;
    a->cpu.cp15.fcse_pid   = 0x02000000u;
    a->cpu.cp15.context_id = 0x00000031u;
    a->cpu.cp15.tpidrurw   = 0xc0ffee00u;
    a->cpu.cp15.tpidruro   = 0xc0ffee04u;
    a->cpu.cp15.tpidrprw   = 0xc0ffee08u;
    for (unsigned i = 0; i < ARM_BANK_COUNT; i++) {
        a->cpu.spsr[i]     = 0x60000010u + i;
        a->cpu.bank_r13[i] = 0x0a000000u + i * 0x100u;
        a->cpu.bank_r14[i] = 0x0b000000u + i * 0x100u;
    }
    for (unsigned i = 0; i < 5; i++) {
        a->cpu.fiq_r8_12[i] = 0x0f000000u + i;
        a->cpu.usr_r8_12[i] = 0x0e000000u + i;
    }
    a->cpu.cycles        = 0x0000000123456789ull;
    a->cpu.abort_pending = true;
    a->cpu.abort_fsr     = 0x0000000du;
    a->cpu.abort_far     = 0xc0001234u;
    a->cpu.irq_line      = true;
    a->cpu.fiq_line      = true;
    a->cpu.excl_valid    = true;
    a->cpu.excl_addr     = 0x08123450u;
    a->cpu.vfp_fpexc     = 0x40000000u;
    a->cpu.vfp_fpscr     = 0x00000010u;
    /* The VFP register file. Distinct per register, because a snapshot that
     * dropped or transposed one would restore a thread whose floating-point
     * state is subtly wrong — the exact failure this file exists to prevent. */
    for (unsigned i = 0; i < 32; i++) a->cpu.vfp_s[i] = 0x5f000000u + i * 0x37u;

    CHECK(roundtrip(a, b), "cpu round trip");

    for (unsigned i = 0; i < 16; i++) SAME(cpu.r[i]);
    SAME(cpu.cpsr);
    SAME(cpu.cp15.sctlr);   SAME(cpu.cp15.actlr);   SAME(cpu.cp15.cpacr);
    SAME(cpu.cp15.ttbr0);   SAME(cpu.cp15.ttbr1);   SAME(cpu.cp15.ttbcr);
    SAME(cpu.cp15.dacr);
    SAME(cpu.cp15.dfsr);    SAME(cpu.cp15.ifsr);
    SAME(cpu.cp15.dfar);    SAME(cpu.cp15.ifar);
    SAME(cpu.cp15.fcse_pid); SAME(cpu.cp15.context_id);
    SAME(cpu.cp15.tpidrurw); SAME(cpu.cp15.tpidruro); SAME(cpu.cp15.tpidrprw);
    for (unsigned i = 0; i < ARM_BANK_COUNT; i++) {
        SAME(cpu.spsr[i]); SAME(cpu.bank_r13[i]); SAME(cpu.bank_r14[i]);
    }
    for (unsigned i = 0; i < 5; i++) { SAME(cpu.fiq_r8_12[i]); SAME(cpu.usr_r8_12[i]); }
    SAME(cpu.cycles);
    SAME(cpu.abort_pending); SAME(cpu.abort_fsr); SAME(cpu.abort_far);
    SAME(cpu.irq_line);      SAME(cpu.fiq_line);
    SAME(cpu.excl_valid);    SAME(cpu.excl_addr);
    SAME(cpu.vfp_fpexc);     SAME(cpu.vfp_fpscr);
    for (unsigned i = 0; i < 32; i++) SAME(cpu.vfp_s[i]);

    /* The bus is host wiring, not guest state: it must point at the restored
     * machine's own bus, not at the machine the snapshot came from. */
    CHECK(b->cpu.bus == &b->bus, "restored cpu->bus must point at its own machine");
    CHECK(b->bus.ctx == b, "restored bus ctx must point at its own machine");

    s5l8900_free(a); s5l8900_free(b);
}

/* ----------------------------------------------------------- device state --- */

static void test_device_state_round_trips(void) {
    s5l8900_t ma, mb;
    s5l8900_t *a = &ma, *b = &mb;
    CHECK(s5l8900_init(a, 0, RAMSZ), "init a");
    CHECK(s5l8900_init(b, 0, RAMSZ), "init b");

    /* UART */
    a->uart0.ulcon = 0x3; a->uart0.ucon = 0x5; a->uart0.ufcon = 0x11;
    a->uart0.umcon = 0x21; a->uart0.ubrdiv = 0x8b;
    memcpy(a->uart0.tx, "panic: whatever", 15);
    a->uart0.tx_len = 15;

    /* Both VICs, including the software-interrupt register. */
    for (unsigned i = 0; i < S5L8900_VIC_COUNT; i++) {
        a->vic[i].raw    = 0x00000080u << i;
        a->vic[i].enable = 0x000000a0u << i;
        a->vic[i].select = 0x00000080u << i;
        a->vic[i].soft   = 0x00000004u << i;
    }

    /* Timer */
    a->timer.ticks     = 0x00000004deadbeefull;
    a->timer.config    = 0x00000003u;
    a->timer.t4_config = 0x000000e0u;
    a->timer.t4_state  = TIMER4_STATE_START;
    a->timer.t4_count  = 60000u;
    a->timer.t4_count2 = 12345u;
    a->timer.t4_value  = 4711u;
    a->timer.irqlatch  = TIMER4_IRQ_BITS;

    /* Power */
    a->power.state = 0x000012fcu; a->power.cfg0 = 1; a->power.cfg1 = 2;
    a->power.sram  = 3; a->power.cfg24 = 4; a->power.cfg28 = 5;

    /* CLCD, every array included. */
    a->clcd.enable = 1; a->clcd.disable = 0; a->clcd.ctrl = CLCD_CTRL_WIN0 | CLCD_CTRL_ENABLE;
    a->clcd.fifo = 0x20202020u;
    a->clcd.intmask = CLCD_INT_FRAME; a->clcd.intstatus = CLCD_INT_FRAME;
    a->clcd.reg1c = 0; a->clcd.preenable = 1; a->clcd.backdrop = 0xff102030u;
    for (unsigned i = 0; i < sizeof a->clcd.video / sizeof a->clcd.video[0]; i++)
        a->clcd.video[i] = 0x5000u + i;
    for (unsigned k = 0; k < CLCD_WIN_COUNT; k++) {
        a->clcd.win[k].stride    = 320 * 4 + k;
        a->clcd.win[k].control   = (CLCD_FMT_32BPP << CLCD_FMT_SHIFT) | k;
        a->clcd.win[k].fbaddr    = 0x0f000000u + k * 0x1000u;
        a->clcd.win[k].geometry  = (320u << 16) | 480u;
        a->clcd.win[k].linewords = 320u;
        a->clcd.win[k].position  = k;
    }
    a->clcd.update = 2; a->clcd.update2 = 0x50001000u;
    for (unsigned i = 0; i < sizeof a->clcd.wincfg_aux / sizeof a->clcd.wincfg_aux[0]; i++)
        a->clcd.wincfg_aux[i] = 0x7000u + i;
    for (unsigned i = 0; i < sizeof a->clcd.csc / sizeof a->clcd.csc[0]; i++)
        a->clcd.csc[i] = 0x8000u + i;
    a->clcd.gate = 1;
    for (unsigned i = 0; i < sizeof a->clcd.opaque / sizeof a->clcd.opaque[0]; i++)
        a->clcd.opaque[i] = 0x9000u + i;
    for (unsigned g = 0; g < 3; g++)
        for (unsigned i = 0; i < 256; i++) a->clcd.gamma[g][i] = g * 0x10000u + i;
    a->clcd.scanning    = true;
    a->clcd.frame_ticks = 100000u;
    a->clcd.frame_accum = 12345u;
    a->clcd.frames      = 0x00000000cafebabeull;

    /* Machine-level diagnostics — guest-visible only indirectly, but state all
     * the same: a restored run that re-reports a device page it already logged
     * has diverged from the run it claims to continue. */
    a->unmapped_reads = 1234; a->unmapped_writes = 5678;
    a->unmapped_addr_count = 3;
    for (unsigned i = 0; i < 3; i++) a->unmapped_addr[i] = 0x40000000u + i * 0x1000u;
    a->trace_devices = true;
    a->dev_count = 4;
    for (unsigned i = 0; i < 4; i++) {
        a->dev_addr[i]     = 0x3cc00020u + i;
        a->dev_value[i]    = 0x41u + i;
        a->dev_is_write[i] = (i & 1) != 0;
    }
    a->cpu_hz = 412000000u; a->tb_hz = 6000000u; a->tb_accum = 1234567u;
    a->stub_declare_failures = 0;

    CHECK(roundtrip(a, b), "device round trip");

    SAME(uart0.ulcon); SAME(uart0.ucon); SAME(uart0.ufcon);
    SAME(uart0.umcon); SAME(uart0.ubrdiv); SAME(uart0.tx_len);
    CHECK(memcmp(a->uart0.tx, b->uart0.tx, UART_TX_BUFFER) == 0,
          "uart tx capture did not survive");

    for (unsigned i = 0; i < S5L8900_VIC_COUNT; i++) {
        SAME(vic[i].raw); SAME(vic[i].enable); SAME(vic[i].select); SAME(vic[i].soft);
    }

    SAME(timer.ticks); SAME(timer.config); SAME(timer.t4_config);
    SAME(timer.t4_state); SAME(timer.t4_count); SAME(timer.t4_count2);
    SAME(timer.t4_value); SAME(timer.irqlatch);

    SAME(power.state); SAME(power.cfg0); SAME(power.cfg1);
    SAME(power.sram);  SAME(power.cfg24); SAME(power.cfg28);

    SAME(clcd.enable); SAME(clcd.disable); SAME(clcd.ctrl); SAME(clcd.fifo);
    SAME(clcd.intmask); SAME(clcd.intstatus); SAME(clcd.reg1c);
    SAME(clcd.preenable); SAME(clcd.backdrop);
    for (unsigned i = 0; i < sizeof a->clcd.video / sizeof a->clcd.video[0]; i++)
        SAME(clcd.video[i]);
    for (unsigned k = 0; k < CLCD_WIN_COUNT; k++) {
        SAME(clcd.win[k].stride);    SAME(clcd.win[k].control);
        SAME(clcd.win[k].fbaddr);    SAME(clcd.win[k].geometry);
        SAME(clcd.win[k].linewords); SAME(clcd.win[k].position);
    }
    SAME(clcd.update); SAME(clcd.update2);
    for (unsigned i = 0; i < sizeof a->clcd.wincfg_aux / sizeof a->clcd.wincfg_aux[0]; i++)
        SAME(clcd.wincfg_aux[i]);
    for (unsigned i = 0; i < sizeof a->clcd.csc / sizeof a->clcd.csc[0]; i++)
        SAME(clcd.csc[i]);
    SAME(clcd.gate);
    for (unsigned i = 0; i < sizeof a->clcd.opaque / sizeof a->clcd.opaque[0]; i++)
        SAME(clcd.opaque[i]);
    CHECK(memcmp(a->clcd.gamma, b->clcd.gamma, sizeof a->clcd.gamma) == 0,
          "clcd gamma LUTs did not survive");
    SAME(clcd.scanning); SAME(clcd.frame_ticks); SAME(clcd.frame_accum);
    SAME(clcd.frames);

    SAME(unmapped_reads); SAME(unmapped_writes); SAME(unmapped_addr_count);
    for (unsigned i = 0; i < 3; i++) SAME(unmapped_addr[i]);
    SAME(trace_devices); SAME(dev_count);
    for (unsigned i = 0; i < 4; i++) {
        SAME(dev_addr[i]); SAME(dev_value[i]); SAME(dev_is_write[i]);
    }
    SAME(cpu_hz); SAME(tb_hz); SAME(tb_accum);
    SAME(stub_declare_failures);

    s5l8900_free(a); s5l8900_free(b);
}

/* The stub windows are honest storage for peripherals we have not modelled.
 * Their contents ARE guest state — a driver that wrote a register and reads it
 * back after a restore must see what it wrote. */
static void test_stub_windows_round_trip(void) {
    s5l8900_t ma, mb;
    s5l8900_t *a = &ma, *b = &mb;
    CHECK(s5l8900_init(a, 0, RAMSZ), "init a");
    CHECK(s5l8900_init(b, 0, RAMSZ), "init b");
    CHECK(a->stub_count > 0, "the machine should declare stub windows");

    a->bus.write32(a->bus.ctx, S5L8900_GPIO_BASE + 0x320u, 0xa5a5a5a5u);
    (void)a->bus.read32(a->bus.ctx, S5L8900_GPIO_BASE + 0x320u);
    a->bus.write32(a->bus.ctx, S5L8900_CLOCK_BASE + 0x404u, 0x00010203u);
    (void)a->bus.read32(a->bus.ctx, S5L8900_MIU_BASE + 0x008u);

    CHECK(roundtrip(a, b), "stub round trip");

    /* Counters first: reading a stub through the bus would bump them. */
    for (unsigned i = 0; i < a->stub_count; i++) {
        SAME(stubs[i].reads); SAME(stubs[i].writes); SAME(stubs[i].oob);
        SAME(stubs[i].nregs); SAME(stubs[i].base); SAME(stubs[i].size);
        /* The backing store is a host allocation; the restore must fill it,
         * never replace it. */
        CHECK(b->stubs[i].regs != NULL, "stub %u lost its backing store", i);
        CHECK(memcmp(a->stubs[i].regs, b->stubs[i].regs,
                     (size_t)a->stubs[i].nregs * 4u) == 0,
              "stub %u backing store differs", i);
    }
    /* And the guest can read back what it wrote before the snapshot. */
    CHECK(b->bus.read32(b->bus.ctx, S5L8900_GPIO_BASE + 0x320u) == 0xa5a5a5a5u,
          "gpio stub register lost its value across a restore");
    CHECK(b->bus.read32(b->bus.ctx, S5L8900_CLOCK_BASE + 0x404u) == 0x00010203u,
          "clock stub register lost its value across a restore");
    s5l8900_free(a); s5l8900_free(b);
}

/* RAM has three page classes; exercise all three plus the ragged tail. */
static void test_ram_round_trips(void) {
    s5l8900_t ma, mb;
    s5l8900_t *a = &ma, *b = &mb;
    CHECK(s5l8900_init(a, 0x08000000u, RAMSZ), "init a");
    CHECK(s5l8900_init(b, 0x08000000u, RAMSZ), "init b");

    memset(a->ram + 0x1000, 0xff, 0x1000);          /* uniform page   */
    for (unsigned i = 0; i < 0x1000; i++)           /* mixed page     */
        a->ram[0x2000 + i] = (uint8_t)(i * 7u + 3u);
    a->ram[RAMSZ - 1] = 0x5a;                       /* very last byte */
    /* page 3 left entirely zero — the class that stores nothing */

    CHECK(roundtrip(a, b), "ram round trip");
    CHECK(memcmp(a->ram, b->ram, RAMSZ) == 0, "guest RAM did not survive");
    /* And the zero pages really are zero, not left over from init. */
    CHECK(b->ram[0x3000] == 0 && b->ram[0x3fff] == 0, "zero page not restored as zero");
    s5l8900_free(a); s5l8900_free(b);
}

/* A guest payload can program the NOR — that is how an untethered jailbreak
 * persists itself — so the flash contents are mutable state, not a constant. */
static void test_nor_round_trips(void) {
    s5l8900_t ma, mb;
    s5l8900_t *a = &ma, *b = &mb;
    CHECK(s5l8900_init(a, 0, RAMSZ), "init a");
    CHECK(s5l8900_init(b, 0, RAMSZ), "init b");

    /* An IMG3 container so the scanned directory is non-empty too. */
    uint8_t img[64];
    memset(img, 0, sizeof img);
    memcpy(img, "3gmI", 4);
    img[4] = 64; img[8] = 40;                       /* fullSize / sizeNoPack */
    memcpy(img + 16, "blli", 4);                    /* ident 'illb' LE       */
    s5l_nor_program(&a->nor, 0x2000, img, sizeof img);
    unsigned found = s5l_nor_scan(&a->nor);
    a->bus.write32(a->bus.ctx, S5L8900_NOR_BASE + 0x8000u, 0x0f0f0f0fu);

    CHECK(roundtrip(a, b), "nor round trip");
    CHECK(memcmp(a->nor.data, b->nor.data, a->nor.size) == 0, "NOR contents differ");
    SAME(nor.size); SAME(nor.image_count);
    for (unsigned i = 0; i < S5L_NOR_MAX_IMAGES; i++) {
        SAME(nor.images[i].ident); SAME(nor.images[i].offset); SAME(nor.images[i].size);
    }
    CHECK(b->nor.image_count == found, "scanned image count %u != %u",
          b->nor.image_count, found);
    s5l8900_free(a); s5l8900_free(b);
}

/* -------------------------------------------------------- the file round --- */

static void test_file_round_trip(void) {
    s5l8900_t ma, mb;
    s5l8900_t *a = &ma, *b = &mb;
    CHECK(s5l8900_init(a, 0, RAMSZ), "init a");
    CHECK(s5l8900_init(b, 0, RAMSZ), "init b");
    a->cpu.r[7] = 0x12345678u;
    a->timer.ticks = 999999ull;
    memset(a->ram + 0x8000, 0x77, 0x4000);

    const char *path = "test_snapshot.tmp";
    snapshot_status_t st = snapshot_save(a, path);
    CHECK(st == SNAP_OK, "file save: %s", snapshot_strerror(st));
    /* The second save exercises atomic replacement of an existing checkpoint
     * (plain rename cannot replace an existing file on Windows). */
    st = snapshot_save(a, path);
    CHECK(st == SNAP_OK, "file replace: %s", snapshot_strerror(st));
    st = snapshot_load(b, path);
    CHECK(st == SNAP_OK, "file load: %s", snapshot_strerror(st));
    SAME(cpu.r[7]); SAME(timer.ticks);
    CHECK(memcmp(a->ram, b->ram, RAMSZ) == 0, "RAM differs after a file round trip");

    /* The file form and the memory form must be the same bytes. */
    uint8_t *mem = NULL; size_t mlen = 0;
    CHECK(snapshot_save_mem(a, &mem, &mlen) == SNAP_OK, "save_mem");
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL, "reopen");
    if (f && mem) {
        uint8_t *fbuf = malloc(mlen + 1);
        size_t got = fread(fbuf, 1, mlen + 1, f);
        CHECK(got == mlen, "file is %zu bytes, memory form is %zu", got, mlen);
        CHECK(got == mlen && memcmp(fbuf, mem, mlen) == 0,
              "file and memory snapshot forms differ");
        free(fbuf);
    }
    if (f) fclose(f);
    free(mem);
    remove(path);
    s5l8900_free(a); s5l8900_free(b);
}

/* ------------------------------------------------------------- rejection --- */

/* A machine that has not been restored into must be exactly as it was. */
static bool machine_is_pristine(s5l8900_t *m) {
    if (m->cpu.r[0] || m->cpu.r[15] || m->cpu.cycles) return false;
    if (m->timer.ticks || m->uart0.tx_len) return false;
    for (uint32_t i = 0; i < RAMSZ; i++) if (m->ram[i]) return false;
    return true;
}

static void test_checksum_valid_malformed_snapshots_are_transactional(void) {
    const uint32_t TAG_CPU_TEST = 0x20555043u; /* "CPU " */
    const uint32_t TAG_NOR_TEST = 0x20524f4eu; /* "NOR " */
    s5l8900_t src;
    CHECK(s5l8900_init(&src, 0, RAMSZ), "init source");
    src.cpu.r[0] = 0xdeadbeefu;
    src.nor.data[0] = 0;
    src.nor.image_count = 1;
    src.nor.images[0].ident = 0x69626f74u;
    src.nor.images[0].offset = 0;
    src.nor.images[0].size = 20;

    uint8_t *good = NULL; size_t glen = 0;
    CHECK(snapshot_save_mem(&src, &good, &glen) == SNAP_OK, "save source");
    if (!good) { s5l8900_free(&src); return; }
    size_t cpu = find_section(good, glen, TAG_CPU_TEST);
    size_t nor = find_section(good, glen, TAG_NOR_TEST);
    CHECK(cpu != 0 && nor != 0, "required sections not found");

    /* A wrong section length used to be noticed only after the CPU visitor
     * had overwritten the live target. Keep the hash valid to reach grammar
     * validation rather than the checksum fast path. */
    {
        uint8_t *bad = malloc(glen);
        memcpy(bad, good, glen);
        wr64le(bad + cpu + 8, rd64le(bad + cpu + 8) + 1u);
        refresh_snapshot_hash(bad, glen);
        s5l8900_t dst; s5l8900_init(&dst, 0, RAMSZ);
        CHECK(snapshot_load_mem(&dst, bad, glen) == SNAP_ERR_CORRUPT,
              "checksum-valid wrong section length was accepted");
        CHECK(machine_is_pristine(&dst) && dst.nor.image_count == 0,
              "rejected section length partially restored the machine");
        s5l8900_free(&dst); free(bad);
    }

    /* One byte after END but inside payload_len was silently ignored. */
    {
        uint64_t plen = rd64le(good + 24);
        size_t trailer = 40u + (size_t)plen;
        uint8_t *bad = malloc(glen + 1u);
        memcpy(bad, good, trailer);
        bad[trailer] = 0xa5;
        memcpy(bad + trailer + 1u, good + trailer, 8u);
        wr64le(bad + 24, plen + 1u);
        refresh_snapshot_hash(bad, glen + 1u);
        s5l8900_t dst; s5l8900_init(&dst, 0, RAMSZ);
        CHECK(snapshot_load_mem(&dst, bad, glen + 1u) == SNAP_ERR_CORRUPT,
              "payload bytes after END were accepted");
        CHECK(machine_is_pristine(&dst), "extra payload byte changed target state");
        s5l8900_free(&dst); free(bad);
    }

    /* Header flags are reserved and outside the payload checksum. */
    {
        uint8_t *bad = malloc(glen);
        memcpy(bad, good, glen); bad[32] = 1;
        s5l8900_t dst; s5l8900_init(&dst, 0, RAMSZ);
        CHECK(snapshot_load_mem(&dst, bad, glen) == SNAP_ERR_CORRUPT,
              "nonzero snapshot header flags were accepted");
        CHECK(machine_is_pristine(&dst), "header-flag rejection changed target state");
        s5l8900_free(&dst); free(bad);
    }

    /* abort_pending is the first serialized CPU boolean, at byte 252 of the
     * CPU body. Booleans have one canonical byte each: 0 or 1. */
    {
        uint8_t *bad = malloc(glen);
        memcpy(bad, good, glen); bad[cpu + 16u + 252u] = 2;
        refresh_snapshot_hash(bad, glen);
        s5l8900_t dst; s5l8900_init(&dst, 0, RAMSZ);
        CHECK(snapshot_load_mem(&dst, bad, glen) == SNAP_ERR_CORRUPT,
              "noncanonical snapshot boolean was accepted");
        CHECK(machine_is_pristine(&dst), "boolean rejection changed target state");
        s5l8900_free(&dst); free(bad);
    }

    /* NOR payload is followed by entries {ident,offset,size}. Keep count=1
     * but push entry 0 past the flash boundary. */
    {
        uint8_t *bad = malloc(glen);
        memcpy(bad, good, glen);
        wr32le(bad + nor + 16u + src.nor.size + 4u, src.nor.size - 10u);
        refresh_snapshot_hash(bad, glen);
        s5l8900_t dst; s5l8900_init(&dst, 0, RAMSZ);
        CHECK(snapshot_load_mem(&dst, bad, glen) == SNAP_ERR_CORRUPT,
              "out-of-range snapshot NOR entry was accepted");
        CHECK(machine_is_pristine(&dst) && dst.nor.image_count == 0 &&
              dst.nor.data[0] == 0xff,
              "NOR-entry rejection partially restored machine state");
        s5l8900_free(&dst); free(bad);
    }

    free(good); s5l8900_free(&src);
}

static void test_bad_snapshots_are_refused(void) {
    s5l8900_t ma;
    s5l8900_t *a = &ma;
    CHECK(s5l8900_init(a, 0, RAMSZ), "init a");
    a->cpu.r[0] = 0xdeadbeefu; a->cpu.cycles = 4242; a->timer.ticks = 77;
    memset(a->ram + 0x400, 0xcd, 0x800);

    uint8_t *good = NULL; size_t glen = 0;
    CHECK(snapshot_save_mem(a, &good, &glen) == SNAP_OK, "save");
    if (!good) { s5l8900_free(a); return; }

    CHECK(snapshot_load_mem(a, NULL, glen) == SNAP_ERR_IO,
          "NULL snapshot bytes were accepted");
    {
        s5l8900_t corrupt_target = *a;
        corrupt_target.stub_count = S5L_STUB_MAX + 1u;
        CHECK(snapshot_load_mem(&corrupt_target, good, glen) == SNAP_ERR_GEOMETRY,
              "corrupt target stub_count reached snapshot traversal");
    }
    {
        uint8_t *out = (uint8_t *)(uintptr_t)1u;
        size_t out_len = 1u;
        CHECK(snapshot_save_mem(NULL, &out, &out_len) == SNAP_ERR_IO &&
              out == NULL && out_len == 0,
              "failed save_mem did not clear its outputs");
    }

    struct { const char *what; snapshot_status_t want; } cases[] = {
        { "bad magic",      SNAP_ERR_MAGIC     },
        { "wrong version",  SNAP_ERR_VERSION   },
        { "truncated",      SNAP_ERR_TRUNCATED },
        { "flipped byte",   SNAP_ERR_CHECKSUM  },
        { "trailing junk",  SNAP_ERR_CORRUPT   },
        { "empty file",     SNAP_ERR_TRUNCATED },
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        uint8_t *bad = malloc(glen + 1);
        memcpy(bad, good, glen);
        size_t blen = glen;
        switch (c) {
            case 0: bad[3] ^= 0xffu; break;                  /* magic        */
            case 1: bad[16] = 99; break;                     /* version      */
            case 2: blen = glen - 1; break;                  /* truncated    */
            case 3: bad[glen - 200] ^= 0x01u; break;         /* payload flip */
            case 4: bad[glen] = 0; blen = glen + 1; break;   /* junk         */
            case 5: blen = 0; break;                         /* nothing      */
        }
        s5l8900_t mb;
        s5l8900_init(&mb, 0, RAMSZ);
        snapshot_status_t st = snapshot_load_mem(&mb, bad, blen);
        CHECK(st == cases[c].want, "%s: got \"%s\", expected \"%s\"",
              cases[c].what, snapshot_strerror(st), snapshot_strerror(cases[c].want));
        CHECK(machine_is_pristine(&mb),
              "%s: the machine was modified by a REFUSED load — a partial "
              "restore is exactly what this must never do", cases[c].what);
        s5l8900_free(&mb);
        free(bad);
    }

    /* A machine with a different RAM size is a different machine. */
    {
        s5l8900_t mb;
        s5l8900_init(&mb, 0, RAMSZ / 2u);
        snapshot_status_t st = snapshot_load_mem(&mb, good, glen);
        CHECK(st == SNAP_ERR_GEOMETRY, "ram-size mismatch: got \"%s\"",
              snapshot_strerror(st));
        CHECK(mb.cpu.r[0] == 0 && mb.cpu.cycles == 0,
              "a geometry-mismatched load must not touch the machine");
        s5l8900_free(&mb);
    }
    /* So is one at a different physical base. */
    {
        s5l8900_t mb;
        s5l8900_init(&mb, 0x08000000u, RAMSZ);
        snapshot_status_t st = snapshot_load_mem(&mb, good, glen);
        CHECK(st == SNAP_ERR_GEOMETRY, "ram-base mismatch: got \"%s\"",
              snapshot_strerror(st));
        s5l8900_free(&mb);
    }

    free(good);
    s5l8900_free(a);
}

/* ------------------------------------------------------------ divergence --- */

/*
 * The unit-scale form of the acceptance test.
 *
 * A guest that spins in a loop while a repeating timer interrupt fires touches
 * the CPU (including banked IRQ registers and the SPSR), the timer, the VIC,
 * the UART and RAM. Snapshot mid-flight, then take the machine forward two
 * ways: continue the original, and continue a restored copy. Snapshot both
 * again and require the two files to be byte-identical.
 *
 * Byte-identical is the right bar rather than "same output": it compares every
 * register, every counter and every page of RAM at once, so a field that is
 * missing from the snapshot shows up here as a difference even if the guest's
 * printable output happens to be unaffected.
 */
static void build_irq_machine(s5l8900_t *m) {
    s5l8900_init(m, 0, RAMSZ);

    const uint32_t branch = 0xea000000u | (((0x40u - 0x18u - 8u) / 4u) & 0x00ffffffu);
    s5l8900_load(m, 0x18, &branch, 4);              /* IRQ vector -> 0x40 */

    /* Handler: print, bump a counter in RAM, acknowledge, return. It leaves
     * the timer running so interrupts keep arriving across the checkpoint. */
    const uint32_t handler[] = {
        0xe3a01054u,   /* MOV r1,#'T'            */
        0xe5801020u,   /* STR r1,[r0,#0x20]      */
        0xe5945000u,   /* LDR r5,[r4]            */
        0xe2855001u,   /* ADD r5,r5,#1           */
        0xe5845000u,   /* STR r5,[r4]            */
        0xe3a01803u,   /* MOV r1,#0x00030000     */
        0xe58210f4u,   /* STR r1,[r2,#0xf4]  ack */
        0xe25ef004u    /* SUBS pc,lr,#4          */
    };
    s5l8900_load(m, 0x40, handler, sizeof handler);

    /* Foreground: a counting spin loop, so RAM keeps changing too. */
    const uint32_t loop[] = {
        0xe2833001u,   /* ADD r3,r3,#1      */
        0xe5863000u,   /* STR r3,[r6]       */
        0xeafffffcu    /* B 0x100           */
    };
    s5l8900_load(m, 0x100, loop, sizeof loop);

    m->cpu_hz = m->tb_hz = 1;      /* one timebase tick per instruction */
    m->bus.write32(m->bus.ctx, S5L8900_VIC0_BASE + VIC_INTENABLE, 1u << S5L8900_IRQ_TIMER);
    m->bus.write32(m->bus.ctx, S5L8900_TIMER_BASE + TIMER4_COUNTBUF, 7);
    m->bus.write32(m->bus.ctx, S5L8900_TIMER_BASE + TIMER4_STATE,
                   TIMER4_STATE_START | TIMER4_STATE_UPDATE);

    m->cpu.r[15] = 0x100;
    m->cpu.r[0]  = S5L8900_UART0_BASE;
    m->cpu.r[2]  = S5L8900_TIMER_BASE;
    m->cpu.r[4]  = 0x200;          /* interrupt counter  */
    m->cpu.r[6]  = 0x204;          /* foreground counter */
    m->cpu.cpsr  = ARM_MODE_SYS;   /* I and F clear */
}

static void test_restore_does_not_diverge(void) {
    s5l8900_t cont, restored;
    build_irq_machine(&cont);

    arm_status_t st = ARM_OK;
    s5l8900_run(&cont, 400, &st);
    CHECK(st == ARM_OK, "checkpoint run status %d", (int)st);

    uint8_t *chk = NULL; size_t chklen = 0;
    CHECK(snapshot_save_mem(&cont, &chk, &chklen) == SNAP_OK, "checkpoint save");

    s5l8900_init(&restored, 0, RAMSZ);
    snapshot_status_t ls = snapshot_load_mem(&restored, chk, chklen);
    CHECK(ls == SNAP_OK, "checkpoint restore: %s", snapshot_strerror(ls));
    free(chk);

    /* Take both machines the same distance forward. */
    s5l8900_run(&cont,     600, &st);
    s5l8900_run(&restored, 600, &st);

    uint8_t *A = NULL, *B = NULL; size_t la = 0, lb = 0;
    CHECK(snapshot_save_mem(&cont,     &A, &la) == SNAP_OK, "final save A");
    CHECK(snapshot_save_mem(&restored, &B, &lb) == SNAP_OK, "final save B");
    CHECK(la == lb, "final snapshots differ in length: %zu vs %zu", la, lb);
    if (A && B && la == lb) {
        size_t at = la;
        for (size_t i = 0; i < la; i++) if (A[i] != B[i]) { at = i; break; }
        CHECK(at == la,
              "RESTORED MACHINE DIVERGED: first difference at byte %zu of %zu "
              "— some machine state is missing from the snapshot", at, la);
    }
    CHECK(cont.cpu.cycles == restored.cpu.cycles, "cycle counts differ");
    CHECK(cont.uart0.tx_len == restored.uart0.tx_len &&
          memcmp(cont.uart0.tx, restored.uart0.tx, cont.uart0.tx_len) == 0,
          "guest console output differs after a restore");
    printf("  [divergence] %llu instructions, %zu bytes of console, "
           "%u interrupts serviced — identical\n",
           (unsigned long long)cont.cpu.cycles, cont.uart0.tx_len,
           (unsigned)cont.ram[0x200]);
    free(A); free(B);
    s5l8900_free(&cont); s5l8900_free(&restored);
}

/* Restoring twice from the same bytes must give the same machine both times —
 * i.e. a load fully overwrites rather than merging into what was there. */
static void test_restore_is_idempotent(void) {
    s5l8900_t cont, b;
    build_irq_machine(&cont);
    arm_status_t st = ARM_OK;
    s5l8900_run(&cont, 400, &st);

    uint8_t *chk = NULL; size_t chklen = 0;
    CHECK(snapshot_save_mem(&cont, &chk, &chklen) == SNAP_OK, "save");

    build_irq_machine(&b);
    s5l8900_run(&b, 137, &st);          /* dirty it differently first */
    CHECK(snapshot_load_mem(&b, chk, chklen) == SNAP_OK, "load 1");
    uint8_t *r1 = NULL; size_t l1 = 0;
    CHECK(snapshot_save_mem(&b, &r1, &l1) == SNAP_OK, "resave 1");
    CHECK(l1 == chklen && r1 && memcmp(r1, chk, chklen) == 0,
          "a restored machine does not re-serialise to the bytes it came from");

    s5l8900_run(&b, 999, &st);
    CHECK(snapshot_load_mem(&b, chk, chklen) == SNAP_OK, "load 2");
    uint8_t *r2 = NULL; size_t l2 = 0;
    CHECK(snapshot_save_mem(&b, &r2, &l2) == SNAP_OK, "resave 2");
    CHECK(l2 == chklen && r2 && memcmp(r2, chk, chklen) == 0,
          "restoring over a dirtied machine left state behind");

    free(chk); free(r1); free(r2);
    s5l8900_free(&cont); s5l8900_free(&b);
}

int main(void) {
    printf("iOS3-VM snapshot tests\n");
    test_cpu_state_round_trips();
    test_device_state_round_trips();
    test_stub_windows_round_trip();
    test_ram_round_trips();
    test_nor_round_trips();
    test_file_round_trip();
    test_bad_snapshots_are_refused();
    test_checksum_valid_malformed_snapshots_are_transactional();
    test_restore_does_not_diverge();
    test_restore_is_idempotent();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

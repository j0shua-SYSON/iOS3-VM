/*
 * iOS3-VM — ARMv6 interpreter unit tests.
 *
 * A tiny dependency-free test harness: a flat 1 MiB RAM behind the arm_bus_t,
 * hand-assembled ARM encodings, single-stepped, with register/flag assertions.
 * Runs identically on Windows (MinGW), Linux, and macOS CI.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- flat memory */
#define RAM_SIZE (1u << 20)
static uint8_t g_ram[RAM_SIZE];

static uint32_t m_r32(void *ctx, uint32_t a){ (void)ctx; uint32_t v; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],4); return v; }
static uint16_t m_r16(void *ctx, uint32_t a){ (void)ctx; uint16_t v; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],2); return v; }
static uint8_t  m_r8 (void *ctx, uint32_t a){ (void)ctx; return g_ram[a&(RAM_SIZE-1)]; }
static void m_w32(void *ctx, uint32_t a, uint32_t v){ (void)ctx; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,4); }
static void m_w16(void *ctx, uint32_t a, uint16_t v){ (void)ctx; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,2); }
static void m_w8 (void *ctx, uint32_t a, uint8_t  v){ (void)ctx; g_ram[a&(RAM_SIZE-1)]=v; }

static const arm_bus_t g_bus = { NULL, m_r32, m_r16, m_r8, m_w32, m_w16, m_w8 };

/* ------------------------------------------------------------- test runner */
static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* Load a sequence of instruction words at 0 and run n steps from PC=0. */
static void load_and_run(arm_cpu_t *c, const uint32_t *prog, size_t words, int steps) {
    memset(g_ram, 0, sizeof g_ram);
    for (size_t i = 0; i < words; i++) m_w32(NULL, (uint32_t)(i*4), prog[i]);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~0x1fu) | ARM_MODE_SYS; /* SYS so all regs are flat */
    for (int i = 0; i < steps; i++) if (arm_step(c) != ARM_OK) break;
}

/* Like load_and_run but returns the status of the last step (for trap tests). */
static arm_status_t run_status(arm_cpu_t *c, const uint32_t *prog, size_t words, int steps) {
    memset(g_ram, 0, sizeof g_ram);
    for (size_t i = 0; i < words; i++) m_w32(NULL, (uint32_t)(i*4), prog[i]);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~0x1fu) | ARM_MODE_SYS;
    arm_status_t st = ARM_OK;
    for (int i = 0; i < steps; i++) { st = arm_step(c); if (st != ARM_OK) break; }
    return st;
}

/* --------------------------------------------------------------- the tests */

static void test_mov_imm(void) {
    /* MOV r0, #42  -> e3a0002a */
    uint32_t p[] = { 0xe3a0002a };
    arm_cpu_t c; load_and_run(&c, p, 1, 1);
    CHECK(c.r[0] == 42, "r0=%u expect 42", c.r[0]);
    CHECK(c.r[15] == 4, "pc=%u expect 4", c.r[15]);
}

static void test_add_reg(void) {
    /* MOV r1,#40 ; MOV r2,#2 ; ADD r0,r1,r2 */
    uint32_t p[] = { 0xe3a01028, 0xe3a02002, 0xe0810002 };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[0] == 42, "r0=%u expect 42", c.r[0]);
}

static void test_sub_flags(void) {
    /* MOV r0,#5 ; SUBS r0,r0,#5  -> Z set, C set (no borrow) */
    uint32_t p[] = { 0xe3a00005, 0xe2500005 };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK(c.r[0] == 0, "r0=%u expect 0", c.r[0]);
    CHECK((c.cpsr & ARM_CPSR_Z) != 0, "Z should be set");
    CHECK((c.cpsr & ARM_CPSR_C) != 0, "C should be set (no borrow)");
    CHECK((c.cpsr & ARM_CPSR_N) == 0, "N should be clear");
}

static void test_subs_negative(void) {
    /* MOV r0,#1 ; SUBS r0,r0,#2 -> result 0xffffffff, N set, C clear (borrow) */
    uint32_t p[] = { 0xe3a00001, 0xe2500002 };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK(c.r[0] == 0xffffffffu, "r0=%08x expect ffffffff", c.r[0]);
    CHECK((c.cpsr & ARM_CPSR_N) != 0, "N should be set");
    CHECK((c.cpsr & ARM_CPSR_C) == 0, "C should be clear (borrow)");
}

static void test_adds_overflow(void) {
    /* r0 = 0x7fffffff ; ADDS r0,r0,#1 -> V set, N set */
    /* MOV r0,#0xff000000-ish is awkward; build 0x7fffffff via MVN r0,#0x80000000? */
    /* Simpler: MOV r0,#0x7f000000 rotated... use two: MVN r0,#0x80000000 gives 0x7fffffff */
    /* MVN r0, #0x80000000 : imm=0x80000000 not encodable directly; use ror.
       0x80000000 = 0x2 ror 2? 0x02 rotated right by 2 -> 0x80000000. rot field=1 (=2*1). */
    uint32_t p[] = { 0xe3e00102, /* MVN r0,#0x80000000 -> r0=0x7fffffff */
                     0xe2900001  /* ADDS r0,r0,#1 */ };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK(c.r[0] == 0x80000000u, "r0=%08x expect 80000000", c.r[0]);
    CHECK((c.cpsr & ARM_CPSR_V) != 0, "V should be set on signed overflow");
    CHECK((c.cpsr & ARM_CPSR_N) != 0, "N should be set");
}

static void test_barrel_lsl(void) {
    /* MOV r1,#1 ; MOV r0, r1, LSL #4 -> 16 */
    uint32_t p[] = { 0xe3a01001, 0xe1a00201 };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK(c.r[0] == 16, "r0=%u expect 16", c.r[0]);
}

static void test_branch(void) {
    /* B +8 (skip one), then MOV r0,#1 (skipped), then MOV r0,#7 */
    /* At pc=0: B to pc=0+8+off. We want to land at word index 2 (addr 8).
       target = pc+8+off = 0+8+off = 8 => off=0 -> encoding e a 000000 */
    uint32_t p[] = { 0xea000000, /* B #8 -> lands at 0x8 */
                     0xe3a00001, /* MOV r0,#1 (skipped) */
                     0xe3a00007  /* MOV r0,#7 */ };
    arm_cpu_t c; load_and_run(&c, p, 3, 2);
    CHECK(c.r[0] == 7, "r0=%u expect 7 (branch skipped the #1)", c.r[0]);
}

static void test_bl_sets_lr(void) {
    /* BL to somewhere; check LR = pc+4 */
    uint32_t p[] = { 0xeb000002 /* BL #... */ };
    arm_cpu_t c; load_and_run(&c, p, 1, 1);
    CHECK(c.r[14] == 4, "lr=%u expect 4", c.r[14]);
}

static void test_ldr_str(void) {
    /* MOV r0,#0xAB ; MOV r1,#0x400 ; STR r0,[r1] ; LDR r2,[r1] */
    uint32_t p[] = { 0xe3a000ab, 0xe3a01b01 /*MOV r1,#0x400*/,
                     0xe5810000 /*STR r0,[r1]*/, 0xe5912000 /*LDR r2,[r1]*/ };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[2] == 0xab, "r2=%08x expect ab", c.r[2]);
    CHECK(m_r32(NULL, 0x400) == 0xab, "mem[0x400]=%08x expect ab", m_r32(NULL,0x400));
}

static void test_ldrb(void) {
    /* Load the word 0x11223344 from a PC-relative literal, store it at 0x500,
     * then LDRB from 0x500 -> 0x44, proving little-endian byte extraction. */
    uint32_t p[] = { 0xe3a01c05 /*MOV  r1,#0x500        addr 0 */,
                     0xe59f0004 /*LDR  r0,[pc,#4]       addr 4 (pc+8=0xC, +4=0x10) */,
                     0xe5810000 /*STR  r0,[r1]          addr 8 */,
                     0xe5d12000 /*LDRB r2,[r1]          addr 0xC */,
                     0x11223344 /*literal               addr 0x10 */ };
    arm_cpu_t c; load_and_run(&c, p, 5, 4);
    CHECK(c.r[0] == 0x11223344u, "r0=%08x expect 11223344", c.r[0]);
    CHECK(c.r[2] == 0x44, "r2=%02x expect 44 (LE low byte)", c.r[2]);
}

static void test_cond_not_taken(void) {
    /* MOVEQ r0,#9 with Z clear -> not executed; r0 stays 0 */
    uint32_t p[] = { 0x03a00009 /*MOVEQ r0,#9*/ };
    arm_cpu_t c; load_and_run(&c, p, 1, 1);
    CHECK(c.r[0] == 0, "r0=%u expect 0 (cond failed)", c.r[0]);
}

static void test_mul(void) {
    /* MOV r1,#6 ; MOV r2,#7 ; MUL r0,r1,r2 -> 42  (MUL rd,rm,rs: e0000291) */
    uint32_t p[] = { 0xe3a01006, 0xe3a02007, 0xe0000291 };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[0] == 42, "r0=%u expect 42", c.r[0]);
}

static void test_orr_bic_mvn(void) {
    /* MOV r0,#0xF0 ; ORR r0,r0,#0x0F -> 0xFF ; BIC r0,r0,#0x0F -> 0xF0 ; MVN r1,#0 -> 0xffffffff */
    uint32_t p[] = { 0xe3a000f0, 0xe380000f, 0xe3c0000f, 0xe3e01000 };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[0] == 0xf0, "r0=%08x expect f0", c.r[0]);
    CHECK(c.r[1] == 0xffffffffu, "r1=%08x expect ffffffff", c.r[1]);
}

static void test_bx_branches(void) {
    /* MOV r1,#0x100 ; BX r1  -> PC = 0x100 (regression: BX must actually branch,
     * not silently execute as a TEQ comparison). */
    uint32_t p[] = { 0xe3a01c01 /*MOV r1,#0x100*/, 0xe12fff11 /*BX r1*/ };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK(c.r[15] == 0x100, "pc=%08x expect 100 (BX branched)", c.r[15]);
}

/*
 * Returning from an exception into Thumb code must land on the halfword the
 * SPSR says, not on the word below it.
 *
 * This one cost a real boot. The kernel's decrementer FIQ interrupts Thumb
 * code roughly half the time at an address that is 2 mod 4. Masking the
 * resume address with ~3 rewinds it by two bytes, so the guest re-executes
 * the *preceding* halfword. In xnu-1357 that turned a zone free into a jump
 * onto the locked path's tail, which then unlocked a mutex whose pointer was
 * a stale scratch value of 1 -- surfacing as a data abort at
 * _lck_mtx_unlock+0x8 with DFAR 0x1, a mile from the actual bug.
 *
 * The tell was statistical: every single one of 761 exception returns landed
 * 4-byte aligned, where hardware would be about half and half.
 */
static void test_exception_return_to_thumb_keeps_halfword(void) {
    uint32_t p[] = { 0xe1b0f00e };   /* MOVS pc, lr */
    arm_cpu_t c; load_and_run(&c, p, 1, 0);   /* load only; we set up state */

    arm_set_mode(&c, ARM_MODE_FIQ);
    c.spsr[arm_bank_of_mode(ARM_MODE_FIQ)] = ARM_MODE_SVC | ARM_CPSR_T;
    c.r[14] = 0x0000101a;            /* Thumb, and 2 mod 4 */
    c.r[15] = 0;
    arm_step(&c);

    CHECK(c.r[15] == 0x0000101a,
          "pc=%08x expect 101a (a ~3 mask would rewind it to 1018)", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_T) != 0, "should have resumed in Thumb state");
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC,
          "mode=%02x expect SVC restored from SPSR",
          c.cpsr & ARM_CPSR_MODE_MASK);
}

/* The same return into ARM code must still be word-aligned. */
static void test_exception_return_to_arm_stays_word_aligned(void) {
    uint32_t p[] = { 0xe1b0f00e };   /* MOVS pc, lr */
    arm_cpu_t c; load_and_run(&c, p, 1, 0);

    arm_set_mode(&c, ARM_MODE_FIQ);
    c.spsr[arm_bank_of_mode(ARM_MODE_FIQ)] = ARM_MODE_SVC;  /* T clear */
    c.r[14] = 0x0000101a;
    c.r[15] = 0;
    arm_step(&c);

    CHECK(c.r[15] == 0x00001018, "pc=%08x expect 1018 (ARM state aligns to 4)",
          c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "should have resumed in ARM state");
}

/*
 * LDM {..., pc}^ is an exception return, and differs from a plain POP {..,pc}
 * in two ways that are easy to get wrong together: bit 0 of the loaded word is
 * part of the address rather than a Thumb selector, and the alignment must
 * follow the T bit the SPSR restores. Doing the PC write before the restore
 * gets both wrong.
 */
static void test_ldm_exception_return_takes_state_from_spsr(void) {
    /* LDMIA sp, {pc}^  ->  0xe8dd8000 */
    uint32_t p[] = { 0xe8dd8000u };
    arm_cpu_t c; load_and_run(&c, p, 1, 0);

    arm_set_mode(&c, ARM_MODE_IRQ);
    c.spsr[arm_bank_of_mode(ARM_MODE_IRQ)] = ARM_MODE_SVC | ARM_CPSR_T;
    c.r[13] = 0x800;
    c.bus->write32(c.bus->ctx, 0x800, 0x0000101au);  /* even: no Thumb bit set */
    c.r[15] = 0;
    arm_step(&c);

    CHECK(c.r[15] == 0x0000101a, "pc=%08x expect 101a (halfword, from SPSR.T)",
          c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_T) != 0,
          "T must come from the SPSR, not from bit 0 of the loaded word");
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC,
          "mode=%02x expect SVC restored from SPSR",
          c.cpsr & ARM_CPSR_MODE_MASK);
}

/* RFE restores PC and CPSR from memory; the same alignment rule applies. */
static void test_rfe_aligns_for_the_restored_state(void) {
    /* RFEIA r0 -> 0xf8900a00 (P=0, U=1: read from [r0], not [r0-4]) */
    uint32_t p[] = { 0xf8900a00u };
    arm_cpu_t c; load_and_run(&c, p, 1, 0);

    arm_set_mode(&c, ARM_MODE_IRQ);
    c.r[0] = 0x900;
    c.bus->write32(c.bus->ctx, 0x900, 0x0000101au);      /* new pc   */
    c.bus->write32(c.bus->ctx, 0x904, ARM_MODE_SVC);     /* new cpsr, T clear */
    c.r[15] = 0;
    arm_step(&c);

    CHECK(c.r[15] == 0x00001018,
          "pc=%08x expect 1018 (ARM state must align to a word)", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "should have resumed in ARM state");
}

/*
 * SETEND LE is a no-op for a little-endian machine and must execute. SETEND BE
 * must keep trapping: we do not model a big-endian data path, so honouring it
 * would silently corrupt every subsequent load.
 */
static void test_setend_le_runs_be_traps(void) {
    uint32_t le[] = { 0xf1010000u, 0xe3a0002au };   /* SETEND LE ; MOV r0,#42 */
    arm_cpu_t c; load_and_run(&c, le, 2, 2);
    CHECK(c.r[0] == 42, "r0=%u expect 42 (SETEND LE must be a no-op)", c.r[0]);

    uint32_t be[] = { 0xf1010200u };                /* SETEND BE */
    arm_cpu_t d; arm_status_t st = run_status(&d, be, 1, 1);
    CHECK(st == ARM_UNDEFINED,
          "status=%d expect ARM_UNDEFINED for SETEND BE", (int)st);
}

/*
 * The 64-bit multiplies. A driver in the real 3.1.3 kernelcache stopped the
 * boot on a plain UMULL, so these are ordinary compiled code rather than an
 * exotic corner. The signed/unsigned distinction is the part worth pinning:
 * doing both in 64-bit unsigned yields the correct low word and a silently
 * wrong high word, which is the kind of error that surfaces a long way away.
 */
static void test_umull_and_smull(void) {
    /* UMULL r0,r1,r2,r3 with 0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE00000001 */
    uint32_t p[] = { 0xe3e02000 /* MVN r2,#0  -> 0xffffffff */,
                     0xe3e03000 /* MVN r3,#0  -> 0xffffffff */,
                     0xe0810392 /* UMULL r0,r1,r2,r3        */ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[0] == 0x00000001u, "lo=%08x expect 00000001", c.r[0]);
    CHECK(c.r[1] == 0xfffffffeu, "hi=%08x expect fffffffe", c.r[1]);

    /* SMULL of the same bit patterns is (-1) * (-1) = 1, so the high word is
     * ZERO. Same inputs, different answer — this is the check that catches an
     * unsigned implementation of the signed form. */
    uint32_t q[] = { 0xe3e02000, 0xe3e03000,
                     0xe0c10392 /* SMULL r0,r1,r2,r3 */ };
    arm_cpu_t d; load_and_run(&d, q, 3, 3);
    CHECK(d.r[0] == 0x00000001u, "lo=%08x expect 00000001 (-1 * -1)", d.r[0]);
    CHECK(d.r[1] == 0x00000000u,
          "hi=%08x expect 00000000 — a signed multiply done unsigned gives "
          "fffffffe here", d.r[1]);
}

static void test_umlal_accumulates(void) {
    /* r0:r1 = 5, then UMLAL r0,r1,r2,r3 with 3 * 7 -> 26 */
    uint32_t p[] = { 0xe3a00005 /* MOV r0,#5 */,
                     0xe3a01000 /* MOV r1,#0 */,
                     0xe3a02003 /* MOV r2,#3 */,
                     0xe3a03007 /* MOV r3,#7 */,
                     0xe0a10392 /* UMLAL r0,r1,r2,r3 */ };
    arm_cpu_t c; load_and_run(&c, p, 5, 5);
    CHECK(c.r[0] == 26, "lo=%u expect 26 (5 + 3*7)", c.r[0]);
    CHECK(c.r[1] == 0,  "hi=%u expect 0", c.r[1]);
}

static void test_clz(void) {
    /* CLZ r0,r1 -> e16f0f11.  Compilers emit this constantly. */
    uint32_t p[] = { 0xe3a01001 /* MOV r1,#1        */, 0xe16f0f11 /* CLZ r0,r1 */,
                     0xe3a01000 /* MOV r1,#0        */, 0xe16f2f11 /* CLZ r2,r1 */,
                     0xe3e01000 /* MVN r1,#0 -> ~0  */, 0xe16f3f11 /* CLZ r3,r1 */ };
    arm_cpu_t c; load_and_run(&c, p, 6, 6);
    CHECK(c.r[0] == 31, "clz(1)=%u expect 31", c.r[0]);
    CHECK(c.r[2] == 32, "clz(0)=%u expect 32 — the case that tempts a loop bug", c.r[2]);
    CHECK(c.r[3] == 0,  "clz(0xffffffff)=%u expect 0", c.r[3]);
}

/*
 * Saturating arithmetic clamps instead of wrapping, and sets the sticky Q
 * flag. Wrapping would be silently correct everywhere except the extremes,
 * which is exactly where these instructions are used.
 */
static void test_qadd_saturates_and_sets_q(void) {
    /* r1 = 0x7fffffff, r2 = 1, QADD r0,r2,r1  (Rd, Rm, Rn) */
    uint32_t p[] = { 0xe3e01102 /* MVN r1,#0x80000000 -> 0x7fffffff */,
                     0xe3a02001 /* MOV r2,#1                        */,
                     0xe1010052 /* QADD r0,r2,r1                    */ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[0] == 0x7fffffffu,
          "r0=%08x expect 7fffffff (clamped, not wrapped to 80000000)", c.r[0]);
    CHECK((c.cpsr & ARM_CPSR_Q) != 0, "Q must be set on saturation");

    /* Without saturation Q must stay clear and the result is ordinary. */
    uint32_t q[] = { 0xe3a01005 /* MOV r1,#5 */, 0xe3a02003 /* MOV r2,#3 */,
                     0xe1010052 /* QADD r0,r2,r1 */ };
    arm_cpu_t d; load_and_run(&d, q, 3, 3);
    CHECK(d.r[0] == 8, "r0=%u expect 8", d.r[0]);
    CHECK((d.cpsr & ARM_CPSR_Q) == 0, "Q must not be set without saturation");
}

static void test_swp_exchanges(void) {
    /* SWP is an atomic read-then-write: Rd gets the old memory word and the
     * new value comes from Rm. Getting the order wrong (writing before
     * reading) silently returns the value just stored, which looks correct
     * whenever Rd and Rm happen to be the same register. */
    uint32_t p[] = { 0xe3a01b02 /* MOV r1,#0x800   */,
                     0xe3a000aa /* MOV r0,#0xAA    */,
                     0xe5810000 /* STR r0,[r1]     seed memory with 0xAA */,
                     0xe3a004bb /* MOV r0,#0xBB000000 */,
                     0xe1012090 /* SWP r2,r0,[r1]  */,
                     0xe5913000 /* LDR r3,[r1]     */ };
    arm_cpu_t c; load_and_run(&c, p, 6, 6);
    CHECK(c.r[2] == 0xaa, "r2=%08x expect aa (SWP returns the OLD word)", c.r[2]);
    CHECK(c.r[3] == 0xbb000000, "r3=%08x expect bb000000 (SWP stored Rm)", c.r[3]);
}

/*
 * LDREXD/STREXD: the doubleword exclusive pair the kernel's 64-bit atomics
 * use. OSAddAtomic64 is the first one a real boot reaches, so this encoding
 * is load-bearing rather than obscure.
 */
static void test_ldrexd_strexd_roundtrip(void) {
    uint32_t p[] = { 0xe3a01b02 /* MOV r1,#0x800        */,
                     0xe3a00012 /* MOV r0,#0x12         */,
                     0xe5810000 /* STR r0,[r1]          low word  */,
                     0xe3a00034 /* MOV r0,#0x34         */,
                     0xe5810004 /* STR r0,[r1,#4]       high word */,
                     0xe1b14f9f /* LDREXD r4,r5,[r1]    */,
                     0xe3a06056 /* MOV r6,#0x56         */,
                     0xe3a07078 /* MOV r7,#0x78         */,
                     0xe1a13f96 /* STREXD r3,r6,r7,[r1] */,
                     0xe5918000 /* LDR r8,[r1]          */,
                     0xe5919004 /* LDR r9,[r1,#4]       */ };
    arm_cpu_t c; load_and_run(&c, p, 11, 11);
    CHECK(c.r[4] == 0x12, "r4=%08x expect 12 (LDREXD low)",  c.r[4]);
    CHECK(c.r[5] == 0x34, "r5=%08x expect 34 (LDREXD high)", c.r[5]);
    CHECK(c.r[3] == 0,    "r3=%08x expect 0 (STREXD succeeded)", c.r[3]);
    CHECK(c.r[8] == 0x56, "r8=%08x expect 56 (STREXD low)",  c.r[8]);
    CHECK(c.r[9] == 0x78, "r9=%08x expect 78 (STREXD high)", c.r[9]);
}

/*
 * CLREX must actually drop the monitor. If it silently does nothing, a STREX
 * that the architecture requires to fail will succeed, and two threads can
 * both believe they hold the same lock.
 */
static void test_clrex_makes_strex_fail(void) {
    uint32_t p[] = { 0xe3a01b02 /* MOV r1,#0x800   */,
                     0xe1910f9f /* LDREX r0,[r1]   */,
                     0xf57ff01f /* CLREX           */,
                     0xe1812f90 /* STREX r2,r0,[r1]*/ };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[2] == 1, "r2=%08x expect 1 (STREX must fail after CLREX)", c.r[2]);
}

/*
 * An exception clears the monitor too. Without this, an interrupt landing
 * between LDREX and STREX leaves the tag intact and the preempted thread's
 * STREX still succeeds -- two owners of one spinlock. Modelled here with SWI,
 * which is the exception we can raise from a test program.
 */
static void test_exception_clears_exclusive_monitor(void) {
    /* Laid out so the SWI vector at 0x08 is part of the same image: branch
     * over it, run LDREX / SWI / STREX, and let the handler return to the
     * STREX via MOV pc,lr. */
    uint32_t p[] = { 0xea000002 /* 0x00 B 0x10        skip the vector */,
                     0x00000000 /* 0x04 (unused)      */,
                     0xe1a0f00e /* 0x08 MOV pc,lr     SWI vector        */,
                     0x00000000 /* 0x0c (unused)      */,
                     0xe3a01b02 /* 0x10 MOV r1,#0x800 */,
                     0xe1910f9f /* 0x14 LDREX r0,[r1] */,
                     0xef000000 /* 0x18 SWI #0        */,
                     0xe1812f90 /* 0x1c STREX r2,r0,[r1] */ };
    arm_cpu_t c; load_and_run(&c, p, 8, 6);
    CHECK(c.r[2] == 1, "r2=%08x expect 1 (STREX must fail after an exception)",
          c.r[2]);
}

static void test_strh_ldrh(void) {
    /* MOV r0,#0x1234-ish via halfword: store 0xAB and read it back as a halfword */
    uint32_t p[] = { 0xe3a000ab /*MOV r0,#0xAB*/,
                     0xe3a01b02 /*MOV r1,#0x800*/,
                     0xe1c100b0 /*STRH r0,[r1]*/,
                     0xe1d120b0 /*LDRH r2,[r1]*/ };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[2] == 0xab, "r2=%08x expect ab (LDRH)", c.r[2]);
}

static void test_ldrsb_sign_extends(void) {
    /* store byte 0xFF, load it signed -> 0xFFFFFFFF */
    uint32_t p[] = { 0xe3a000ff /*MOV r0,#0xFF*/,
                     0xe3a01b02 /*MOV r1,#0x800*/,
                     0xe5c10000 /*STRB r0,[r1]*/,
                     0xe1d120d0 /*LDRSB r2,[r1]*/ };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[2] == 0xffffffffu, "r2=%08x expect ffffffff (LDRSB)", c.r[2]);
}

static void test_ldrsh_sign_extends(void) {
    /* store halfword 0x8000, load it signed -> 0xFFFF8000 */
    uint32_t p[] = { 0xe3a00902 /*MOV r0,#0x8000*/,
                     0xe3a01b02 /*MOV r1,#0x800*/,
                     0xe1c100b0 /*STRH r0,[r1]*/,
                     0xe1d120f0 /*LDRSH r2,[r1]*/ };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[2] == 0xffff8000u, "r2=%08x expect ffff8000 (LDRSH)", c.r[2]);
}

static void test_stmia_ldmia(void) {
    /* STMIA r1!,{r0,r2} then LDMIA r1,{r3,r4} round-trips both words. */
    uint32_t p[] = { 0xe3a00011 /*MOV r0,#0x11*/,
                     0xe3a02022 /*MOV r2,#0x22*/,
                     0xe3a01b02 /*MOV r1,#0x800*/,
                     0xe8a10005 /*STMIA r1!,{r0,r2}*/,
                     0xe3a01b02 /*MOV r1,#0x800*/,
                     0xe8910018 /*LDMIA r1,{r3,r4}*/ };
    arm_cpu_t c; load_and_run(&c, p, 6, 6);
    CHECK(c.r[3] == 0x11, "r3=%08x expect 11", c.r[3]);
    CHECK(c.r[4] == 0x22, "r4=%08x expect 22", c.r[4]);
}

static void test_push_pop(void) {
    /* The classic prologue/epilogue pair: STMDB sp!,{r0,r1} / LDMIA sp!,{r2,r3}.
     * Also checks the stack pointer returns to where it started. */
    uint32_t p[] = { 0xe3a0dc09 /*MOV sp,#0x900*/,
                     0xe3a000aa /*MOV r0,#0xAA*/,
                     0xe3a010bb /*MOV r1,#0xBB*/,
                     0xe92d0003 /*STMDB sp!,{r0,r1}  (push)*/,
                     0xe8bd000c /*LDMIA sp!,{r2,r3}  (pop)*/ };
    arm_cpu_t c; load_and_run(&c, p, 5, 5);
    CHECK(c.r[2] == 0xaa, "r2=%08x expect aa", c.r[2]);
    CHECK(c.r[3] == 0xbb, "r3=%08x expect bb", c.r[3]);
    CHECK(c.r[13] == 0x900, "sp=%08x expect 900 (balanced)", c.r[13]);
}

static void test_ldm_to_pc_branches(void) {
    /* LDMIA sp!,{pc} must branch. Push 0x300 then pop it into PC. */
    uint32_t p[] = { 0xe3a0dc09 /*MOV sp,#0x900*/,
                     0xe3a00c03 /*MOV r0,#0x300*/,
                     0xe92d0001 /*STMDB sp!,{r0}*/,
                     0xe8bd8000 /*LDMIA sp!,{pc}*/ };
    arm_cpu_t c; load_and_run(&c, p, 4, 4);
    CHECK(c.r[15] == 0x300, "pc=%08x expect 300 (LDM to PC branched)", c.r[15]);
}

static void test_imm_dp_not_trapped(void) {
    /* Regression for the extra-load/store mask: MOV r0,#0x90 has imm8 bits 7 and
     * 4 set, but is immediate data-processing (bit25=1) and must execute. */
    uint32_t p[] = { 0xe3a00090 /*MOV r0,#0x90*/ };
    arm_cpu_t c; arm_status_t st = run_status(&c, p, 1, 1);
    CHECK(st == ARM_OK, "status=%d expect ARM_OK for MOV imm", (int)st);
    CHECK(c.r[0] == 0x90, "r0=%08x expect 90", c.r[0]);
}

static void test_banked_sp_per_mode(void) {
    /* r13 is banked per mode: a value written in SVC must not be visible in IRQ,
     * and must come back when we switch back. */
    arm_cpu_t c; arm_reset(&c, &g_bus);
    arm_set_mode(&c, ARM_MODE_SVC);
    c.r[13] = 0xAAAA0000u;
    arm_set_mode(&c, ARM_MODE_IRQ);
    CHECK(c.r[13] != 0xAAAA0000u, "sp_irq leaked sp_svc (%08x)", c.r[13]);
    c.r[13] = 0xBBBB0000u;
    arm_set_mode(&c, ARM_MODE_SVC);
    CHECK(c.r[13] == 0xAAAA0000u, "sp_svc=%08x expect aaaa0000 after switch back", c.r[13]);
    arm_set_mode(&c, ARM_MODE_IRQ);
    CHECK(c.r[13] == 0xBBBB0000u, "sp_irq=%08x expect bbbb0000", c.r[13]);
}

static void test_fiq_banks_r8_r12(void) {
    /* FIQ additionally banks r8-r12. */
    arm_cpu_t c; arm_reset(&c, &g_bus);
    arm_set_mode(&c, ARM_MODE_SVC);
    c.r[8] = 0x1234;
    arm_set_mode(&c, ARM_MODE_FIQ);
    CHECK(c.r[8] != 0x1234, "r8_fiq leaked r8_usr (%08x)", c.r[8]);
    c.r[8] = 0x5678;
    arm_set_mode(&c, ARM_MODE_SVC);
    CHECK(c.r[8] == 0x1234, "r8=%08x expect 1234 restored", c.r[8]);
}

static void test_mrs_reads_cpsr(void) {
    /* MRS r0, CPSR */
    uint32_t p[] = { 0xe10f0000 };
    arm_cpu_t c; load_and_run(&c, p, 1, 1);
    CHECK(c.r[0] == c.cpsr, "r0=%08x expect cpsr=%08x", c.r[0], c.cpsr);
}

static void test_msr_switches_mode(void) {
    /* MOV r0,#0xD2 (IRQ mode, I set) ; MSR CPSR_c, r0 -> mode becomes IRQ */
    uint32_t p[] = { 0xe3a000d2 /*MOV r0,#0xD2*/,
                     0xe121f000 /*MSR CPSR_c, r0*/ };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_IRQ,
          "mode=%02x expect IRQ(12)", c.cpsr & ARM_CPSR_MODE_MASK);
}

static void test_swi_enters_svc(void) {
    /* SWI #0 from SYS mode: vectors to 0x08, enters SVC, LR=return, SPSR=old. */
    uint32_t p[] = { 0xef000000 /*SWI #0*/ };
    arm_cpu_t c; load_and_run(&c, p, 1, 1);
    CHECK(c.r[15] == ARM_VEC_SWI, "pc=%08x expect 08 (SWI vector)", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC,
          "mode=%02x expect SVC(13)", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.r[14] == 4, "lr_svc=%08x expect 4 (return address)", c.r[14]);
    CHECK((c.cpsr & ARM_CPSR_I) != 0, "IRQs should be masked on exception entry");
    CHECK((c.spsr[ARM_BANK_SVC] & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS,
          "spsr_svc should record the interrupted SYS mode");
}

static void test_exception_return_restores_mode(void) {
    /* Full round trip: SWI from SYS lands in SVC; the handler pushes LR and
     * returns with LDMIA sp!,{pc}^ which restores CPSR from SPSR (back to SYS). */
    uint32_t p[16] = {0};
    p[0] = 0xef000000;                 /* 0x00: SWI #0 -> vectors to 0x08 */
    p[2] = 0xe3a0dc09;                 /* 0x08: MOV sp,#0x900 (sp_svc)    */
    p[3] = 0xe92d4000;                 /* 0x0c: STMDB sp!,{lr}            */
    p[4] = 0xe8fd8000;                 /* 0x10: LDMIA sp!,{pc}^  (return) */
    arm_cpu_t c; load_and_run(&c, p, 16, 4);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS,
          "mode=%02x expect back in SYS(1f)", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.r[15] == 4, "pc=%08x expect 4 (returned after the SWI)", c.r[15]);
}

static void test_cp15_reads_midr(void) {
    /* MRC p15,0,r0,c0,c0,0 -> the ARM1176JZF-S main ID. The kernel reads this
     * to identify the CPU it is running on. */
    uint32_t p[] = { 0xee100f10 };
    arm_cpu_t c; load_and_run(&c, p, 1, 1);
    CHECK(c.r[0] == ARM1176_MIDR, "r0=%08x expect %08x (MIDR)", c.r[0], ARM1176_MIDR);
}

static void test_cp15_cpuid_feature_bank(void) {
    /* CP15 c0 CRm=1 and CRm=2 are the ARMv6 CPUID feature identification
     * registers. MIDR[19:16] on the ARM1176JZF-S reads 0xF, which means
     * "the architecture version is described by this bank, not by MIDR" —
     * so a kernel that wants to know what it is running on has to read it. */
    uint32_t p[] = { 0xee100f11 /*MRC p15,0,r0,c0,c1,0  ID_PFR0 */,
                     0xee101f91 /*MRC p15,0,r1,c0,c1,4  ID_MMFR0*/,
                     0xee102f12 /*MRC p15,0,r2,c0,c2,0  ID_ISAR0*/,
                     0xee103f32 /*MRC p15,0,r3,c0,c2,1  ID_ISAR1*/,
                     0xee104f92 /*MRC p15,0,r4,c0,c2,4  ID_ISAR4*/ };
    arm_cpu_t c; load_and_run(&c, p, 5, 5);
    CHECK(c.r[0] == ARM1176_ID_PFR0,  "ID_PFR0=%08x expect %08x",  c.r[0], ARM1176_ID_PFR0);
    CHECK(c.r[1] == ARM1176_ID_MMFR0, "ID_MMFR0=%08x expect %08x", c.r[1], ARM1176_ID_MMFR0);
    CHECK(c.r[2] == ARM1176_ID_ISAR0, "ID_ISAR0=%08x expect %08x", c.r[2], ARM1176_ID_ISAR0);
    CHECK(c.r[3] == ARM1176_ID_ISAR1, "ID_ISAR1=%08x expect %08x", c.r[3], ARM1176_ID_ISAR1);
    CHECK(c.r[4] == ARM1176_ID_ISAR4, "ID_ISAR4=%08x expect %08x", c.r[4], ARM1176_ID_ISAR4);
}

static void test_cp15_cpuid_scheme_grades_as_armv6(void) {
    /*
     * This is xnu-1357.5.30's do_cpuid(), lifted verbatim from the kernel at
     * 0xc006257c and run against our CP15. It reads MIDR, notices the 0xF
     * architecture field, reads ID_ISAR1, and if the Jazelle field says 2 it
     * rewrites the architecture field to 7 (ARMv6).
     *
     * cpu_init() then indexes a jump table with (arch - 2) and stores
     * CPU_SUBTYPE_ARM_V6 for arch 7; anything it does not recognise stores
     * CPU_SUBTYPE_ARM_ALL (0), and grade_binary() rejects every ARMv6 Mach-O
     * on the disk with EBADARCH. If this assertion ever fails again, /sbin/launchd
     * stops exec'ing.
     */
    uint32_t p[] = { 0xee101f10 /*MRC p15,0,r1,c0,c0,0   MIDR         */,
                     0xee103f32 /*MRC p15,0,r3,c0,c2,1   ID_ISAR1     */,
                     0xe1a03423 /*LSR   r3, r3, #8                    */,
                     0xe20330f0 /*AND   r3, r3, #0xf0                 */,
                     0xe3530020 /*CMP   r3, #0x20        Jazelle == 2 */,
                     0x03c13702 /*BICEQ r3, r1, #0x80000              */,
                     0x03833807 /*ORREQ r3, r3, #0x70000              */ };
    arm_cpu_t c; load_and_run(&c, p, 7, 7);
    CHECK(((c.r[1] >> 16) & 0xfu) == 0xfu,
          "MIDR arch nibble=%x expect f (CPUID scheme)", (c.r[1] >> 16) & 0xfu);
    CHECK(((c.r[3] >> 16) & 0xfu) == 7u,
          "fixed-up arch nibble=%x expect 7 (ARMv6) — cpu_subtype would be ARM_ALL",
          (c.r[3] >> 16) & 0xfu);
}

static void test_cp15_id_dfr0_matches_absent_debug_unit(void) {
    /* We do not model the CP14 debug unit, so DBGDIDR reads zero. ID_DFR0 must
     * agree and report "no debug architecture": XNU's do_debugid() treats a
     * non-zero ID_DFR0 as licence to publish a breakpoint count derived from
     * DBGDIDR, so the two answers have to be consistent with each other. */
    uint32_t p[] = { 0xee100f51 /*MRC p15,0,r0,c0,c1,2   ID_DFR0 */,
                     0xee101e10 /*MRC p14,0,r1,c0,c0,0   DBGDIDR */ };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK(c.r[0] == 0, "ID_DFR0=%08x expect 0 (no debug unit modelled)", c.r[0]);
    CHECK(c.r[1] == 0, "DBGDIDR=%08x expect 0", c.r[1]);
}

static void test_cp15_sctlr_roundtrip(void) {
    /* Deliberately write SCTLR.C (data cache) rather than SCTLR.M: setting M
     * would switch the MMU on mid-program and, with no page tables loaded, the
     * very next fetch would legitimately take a prefetch abort. */
    uint32_t p[] = { 0xe3a00004 /*MOV r0,#4 (SCTLR.C)*/,
                     0xee010f10 /*MCR p15,0,r0,c1,c0,0*/,
                     0xee111f10 /*MRC p15,0,r1,c1,c0,0*/ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[1] == 4, "r1=%08x expect 4 (SCTLR readback)", c.r[1]);
    CHECK((c.cp15.sctlr & ARM_SCTLR_C) != 0, "SCTLR.C should be set");
}

static void test_cp15_ttbr0_roundtrip(void) {
    /* Translation table base survives a write/read cycle (needed for the MMU). */
    uint32_t p[] = { 0xe3a00b02 /*MOV r0,#0x800*/,
                     0xee020f10 /*MCR p15,0,r0,c2,c0,0  (TTBR0)*/,
                     0xee121f10 /*MRC p15,0,r1,c2,c0,0*/ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.cp15.ttbr0 == 0x800, "ttbr0=%08x expect 800", c.cp15.ttbr0);
    CHECK(c.r[1] == 0x800, "r1=%08x expect 800", c.r[1]);
}

static void test_cp15_cache_op_is_accepted(void) {
    /* MCR p15,0,r0,c7,c5,0 (invalidate I-cache) must not trap. */
    uint32_t p[] = { 0xee070f15 };
    arm_cpu_t c; arm_status_t st = run_status(&c, p, 1, 1);
    CHECK(st == ARM_OK, "status=%d expect ARM_OK for cache maintenance", (int)st);
}

static void test_high_vectors(void) {
    /* Setting SCTLR.V (bit 13) moves the vector table to 0xFFFF0000, so a SWI
     * must vector to 0xFFFF0008 instead of 0x8. */
    uint32_t p[] = { 0xe3a00a02 /*MOV r0,#0x2000 (SCTLR.V)*/,
                     0xee010f10 /*MCR p15,0,r0,c1,c0,0*/,
                     0xef000000 /*SWI #0*/ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[15] == 0xffff0008u, "pc=%08x expect ffff0008 (high vectors)", c.r[15]);
}

/* ---------------------------------------------------------------- MMU tests */
/* Build a first-level table at 0x4000 mapping one 1 MB section, exactly as a
 * kernel would lay it out in RAM, then let the real walker translate. */
static void mmu_setup_section(arm_cpu_t *c, uint32_t va, uint32_t pa,
                              unsigned ap, unsigned domain) {
    const uint32_t l1 = 0x4000;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, l1 + ((va >> 20) << 2),
          (pa & 0xfff00000u) | (ap << 10) | (domain << 5) | 2u); /* section */
    arm_reset(c, &g_bus);
    c->cp15.ttbr0 = l1;
    c->cp15.dacr  = 1u << (domain * 2);       /* client: check AP */
    c->cp15.sctlr |= ARM_SCTLR_M;             /* MMU on */
}

static void test_mmu_disabled_is_identity(void) {
    arm_cpu_t c; arm_reset(&c, &g_bus);
    uint32_t pa = 0;
    uint32_t f = arm_mmu_translate(&c, 0xdeadb000u, ARM_ACCESS_READ, true, &pa);
    CHECK(f == 0, "fsr=%u expect 0 with MMU off", f);
    CHECK(pa == 0xdeadb000u, "pa=%08x expect identity", pa);
}

static void test_mmu_section_translation(void) {
    arm_cpu_t c; mmu_setup_section(&c, 0x80000000u, 0x00200000u, 3, 0);
    uint32_t pa = 0;
    uint32_t f = arm_mmu_translate(&c, 0x80001234u, ARM_ACCESS_READ, true, &pa);
    CHECK(f == 0, "fsr=%u expect 0", f);
    CHECK(pa == 0x00201234u, "pa=%08x expect 00201234", pa);
}

static void test_mmu_unmapped_faults(void) {
    arm_cpu_t c; mmu_setup_section(&c, 0x80000000u, 0x00200000u, 3, 0);
    uint32_t pa = 0;
    /* 0x90000000 has no descriptor at all. */
    uint32_t f = arm_mmu_translate(&c, 0x90000000u, ARM_ACCESS_READ, true, &pa);
    CHECK((f & 0xf) == ARM_FSR_SECTION_TRANSLATION,
          "fsr=%x expect section translation fault", f);
}

static void test_mmu_user_write_permission(void) {
    /* AP=10: privileged RW, user read-only. A user write must fault. */
    arm_cpu_t c; mmu_setup_section(&c, 0x80000000u, 0x00200000u, 2, 0);
    uint32_t pa = 0;
    CHECK(arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, false, &pa) == 0,
          "user read should be permitted with AP=10");
    uint32_t f = arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_WRITE, false, &pa);
    CHECK((f & 0xf) == ARM_FSR_SECTION_PERMISSION,
          "fsr=%x expect permission fault on user write", f);
}

static void test_mmu_small_page_translation(void) {
    /* Two-level walk: L1 coarse pointer -> L2 small page. */
    const uint32_t l1 = 0x4000, l2 = 0x5000;
    arm_cpu_t c;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, l1 + ((0x80000000u >> 20) << 2), (l2 & 0xfffffc00u) | 1u); /* coarse */
    m_w32(NULL, l2 + (((0x80000000u >> 12) & 0xff) << 2),
          (0x00300000u & 0xfffff000u) | (3u << 4) | 2u);                   /* small page, AP=11 */
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = l1; c.cp15.dacr = 1u; c.cp15.sctlr |= ARM_SCTLR_M;
    uint32_t pa = 0;
    uint32_t f = arm_mmu_translate(&c, 0x80000abcu, ARM_ACCESS_READ, true, &pa);
    CHECK(f == 0, "fsr=%u expect 0 for small page", f);
    CHECK(pa == 0x00300abcu, "pa=%08x expect 00300abc", pa);
}

static void test_data_abort_taken(void) {
    /* With the MMU on and nothing mapped at the load address, LDR must raise a
     * data abort: ABT mode, vector 0x10, and DFAR recording the address. */
    arm_cpu_t c;
    memset(g_ram, 0, sizeof g_ram);
    /* identity-map the low section so the fetch itself succeeds */
    m_w32(NULL, 0x4000 + 0, (0x00000000u) | (3u << 10) | 2u);
    /* 0x90000000 is in a different 1 MB section than the identity-mapped one,
     * so it has no descriptor at all. */
    uint32_t prog[] = { 0xe3a01209 /*MOV r1,#0x90000000*/, 0xe5912000 /*LDR r2,[r1]*/ };
    for (unsigned i = 0; i < 2; i++) m_w32(NULL, i * 4, prog[i]);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.cp15.ttbr0 = 0x4000; c.cp15.dacr = 1u; c.cp15.sctlr |= ARM_SCTLR_M;
    arm_step(&c); arm_step(&c);
    CHECK(c.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (data abort)", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_ABT,
          "mode=%02x expect ABT(17)", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.cp15.dfar == 0x90000000u, "dfar=%08x expect 90000000", c.cp15.dfar);
}

/* --- regressions from the adversarial audit ----------------------------- */

static void test_pld_is_a_nop_not_a_branch(void) {
    /* cond==0xF is the ARMv6 unconditional space. PLD must be a no-op; if it is
     * decoded as a conditional instruction it becomes "LDRB pc,[r1]" and
     * branches to whatever byte it loaded. */
    uint32_t p[] = { 0xe3a01a01 /*MOV r1,#0x1000*/,
                     0xf5d1f000 /*PLD [r1]*/,
                     0xe3a00007 /*MOV r0,#7*/ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[0] == 7, "r0=%u expect 7 (execution continued past PLD)", c.r[0]);
    CHECK(c.r[15] == 12, "pc=%08x expect 0c (PLD did not branch)", c.r[15]);
}

static void test_unconditional_space_traps(void) {
    /* Unimplemented cond==0xF encodings (e.g. SETEND) must trap, not execute. */
    uint32_t p[] = { 0xf1010200 /*SETEND BE*/ };
    arm_cpu_t c; arm_status_t st = run_status(&c, p, 1, 1);
    CHECK(st == ARM_UNDEFINED, "status=%d expect ARM_UNDEFINED for SETEND", (int)st);
}

static void test_clz_does_not_corrupt_cpsr(void) {
    /* CLZ sits in the same opcode space as MSR. With a loose MSR mask it would
     * rewrite CPSR (including the mode field) instead of trapping. */
    uint32_t p[] = { 0xe16f0f11 /*CLZ r0,r1*/ };
    arm_cpu_t c; arm_reset(&c, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, p[0]);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.r[1] = 1;
    uint32_t before = c.cpsr;
    arm_status_t st = arm_step(&c);
    CHECK(st == ARM_OK, "status=%d expect ARM_OK — CLZ is implemented", (int)st);
    CHECK(c.r[0] == 31, "r0=%u expect 31 (clz of 1)", c.r[0]);
    CHECK(c.cpsr == before, "cpsr changed %08x -> %08x (CLZ decoded as MSR)",
          before, c.cpsr);
}

static void test_msr_still_works(void) {
    /* The tightened mask must not break real MSR. */
    uint32_t p[] = { 0xe3a000d2 /*MOV r0,#0xD2*/, 0xe121f000 /*MSR CPSR_c,r0*/ };
    arm_cpu_t c; load_and_run(&c, p, 2, 2);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_IRQ,
          "mode=%02x expect IRQ", c.cpsr & ARM_CPSR_MODE_MASK);
}

static void test_arm_media_extend_and_reverse(void) {
    /* The ARM media extend and byte-reverse families, which real XNU uses in
     * ordinary compiled code. (These used to trap; that assertion documented a
     * limitation that no longer exists.) */
    uint32_t p[] = { 0xe59f1010 /*LDR r1,[pc,#16] -> the literal at 0x18*/,
                     0xe6bf0f31 /*REV  r0,r1 */,
                     0xe6ef2071 /*UXTB r2,r1 */,
                     0xe6bf3071 /*SXTH r3,r1 */,
                     0xe6ff4071 /*UXTH r4,r1 */,
                     0xeafffffe /*B .        */,
                     0x11228344 /*literal    */ };
    arm_cpu_t c; load_and_run(&c, p, 7, 5);
    CHECK(c.r[1] == 0x11228344u, "r1=%08x expect 11228344", c.r[1]);
    CHECK(c.r[0] == 0x44832211u, "r0=%08x expect 44832211 (REV)", c.r[0]);
    CHECK(c.r[2] == 0x44, "r2=%08x expect 44 (UXTB)", c.r[2]);
    CHECK(c.r[3] == 0xffff8344u, "r3=%08x expect ffff8344 (SXTH sign-extends)", c.r[3]);
    CHECK(c.r[4] == 0x8344, "r4=%08x expect 8344 (UXTH)", c.r[4]);
}

static void test_unimplemented_media_still_traps(void) {
    /* SEL is media-space but not implemented; it must still be named rather
     * than silently mis-executed as a load/store. */
    uint32_t sel[] = { 0xe6800fb1 };   /* SEL r0,r0,r1 */
    arm_cpu_t c; arm_status_t st = run_status(&c, sel, 1, 1);
    CHECK(st == ARM_UNDEFINED, "status=%d expect ARM_UNDEFINED for SEL", (int)st);
}

static void test_apx_makes_mapping_read_only(void) {
    /* APX=1, AP=01 is privileged read-only: a privileged write must fault. */
    arm_cpu_t c;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x4000 + ((0x80000000u >> 20) << 2),
          0x00200000u | (1u << 15) | (1u << 10) | 2u);   /* APX=1, AP=01 */
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = 0x4000; c.cp15.dacr = 1u; c.cp15.sctlr |= ARM_SCTLR_M;

    uint32_t pa = 0;
    CHECK(arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_READ, true, &pa) == 0,
          "privileged read should be allowed with APX=1,AP=01");
    uint32_t f = arm_mmu_translate(&c, 0x80000000u, ARM_ACCESS_WRITE, true, &pa);
    CHECK((f & 0xf) == ARM_FSR_SECTION_PERMISSION,
          "fsr=%x expect permission fault on write to a read-only section", f);
}

static void test_abort_restores_base_register(void) {
    /* Base Restored Abort Model: after a data abort the base and destination
     * registers must be unchanged so the handler can retry the instruction. */
    arm_cpu_t c;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x4000 + 0, 0x00000000u | (3u << 10) | 2u);   /* identity map VA 0 */
    uint32_t prog[] = { 0xe4901004 };    /* LDR r1,[r0],#4  (post-indexed) */
    m_w32(NULL, 0, prog[0]);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.cp15.ttbr0 = 0x4000; c.cp15.dacr = 1u; c.cp15.sctlr |= ARM_SCTLR_M;
    c.r[0] = 0x00100000;                 /* unmapped */
    c.r[1] = 0x5a5a5a5a;
    arm_step(&c);
    CHECK(c.r[0] == 0x00100000u, "r0=%08x expect 00100000 (base restored)", c.r[0]);
    CHECK(c.r[1] == 0x5a5a5a5au, "r1=%08x expect 5a5a5a5a (dest untouched)", c.r[1]);
    CHECK(c.cp15.dfar == 0x00100000u, "dfar=%08x expect 00100000", c.cp15.dfar);
}

/* --------------------------------------------------------------- Thumb --- */

/* Load 16-bit Thumb halfwords at `base` and run from there in Thumb state. */
static void load_thumb(arm_cpu_t *c, const uint16_t *prog, size_t n, int steps) {
    memset(g_ram, 0, sizeof g_ram);
    for (size_t i = 0; i < n; i++) m_w16(NULL, (uint32_t)(i*2), prog[i]);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~0x1fu) | ARM_MODE_SYS;
    c->cpsr |= ARM_CPSR_T;
    c->r[15] = 0;
    for (int i = 0; i < steps; i++) if (arm_step(c) != ARM_OK) break;
}

static void test_thumb_mov_add(void) {
    /* MOV r0,#40 ; MOV r1,#2 ; ADD r0,r0,r1 */
    uint16_t p[] = { 0x2028, 0x2102, 0x1840 };
    arm_cpu_t c; load_thumb(&c, p, 3, 3);
    CHECK(c.r[0] == 42, "r0=%u expect 42", c.r[0]);
    CHECK(c.r[15] == 6, "pc=%u expect 6 (2 bytes per instruction)", c.r[15]);
}

static void test_thumb_lsl_flags(void) {
    /* MOV r0,#1 ; LSL r1,r0,#31 -> N set */
    uint16_t p[] = { 0x2001, 0x07c1 };
    arm_cpu_t c; load_thumb(&c, p, 2, 2);
    CHECK(c.r[1] == 0x80000000u, "r1=%08x expect 80000000", c.r[1]);
    CHECK((c.cpsr & ARM_CPSR_N) != 0, "N should be set");
}

static void test_thumb_push_pop(void) {
    /* SP=0x900 ; r0=0xAA ; r1=0xBB ; PUSH {r0,r1} ; POP {r2,r3} */
    uint16_t p[] = { 0x20aa,        /* MOV r0,#0xAA */
                     0x21bb,        /* MOV r1,#0xBB */
                     0xb403,        /* PUSH {r0,r1} */
                     0xbc0c };      /* POP  {r2,r3} */
    arm_cpu_t c; load_thumb(&c, p, 4, 0);
    c.r[13] = 0x900;
    for (int i = 0; i < 4; i++) arm_step(&c);
    CHECK(c.r[2] == 0xaa, "r2=%08x expect aa", c.r[2]);
    CHECK(c.r[3] == 0xbb, "r3=%08x expect bb", c.r[3]);
    CHECK(c.r[13] == 0x900, "sp=%08x expect 900 (balanced)", c.r[13]);
}

static void test_thumb_conditional_branch(void) {
    /* MOV r0,#1 ; CMP r0,#1 ; BNE +4 (not taken) ; MOV r1,#7 */
    uint16_t p[] = { 0x2001, 0x2801, 0xd101, 0x2107 };
    arm_cpu_t c; load_thumb(&c, p, 4, 4);
    CHECK(c.r[1] == 7, "r1=%u expect 7 (BNE not taken)", c.r[1]);
}

static void test_arm_to_thumb_and_back(void) {
    /* ARM: set r0 = 0x10|1 and BX to it, landing in Thumb at 0x10.
     * Thumb at 0x10: MOV r1,#5 ; then BX r2 with r2 = 0x20 (back to ARM).
     * ARM at 0x20: MOV r3,#9. */
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0x00, 0xe3a00011);      /* MOV r0,#0x11        */
    m_w32(NULL, 0x04, 0xe3a02020);      /* MOV r2,#0x20        */
    m_w32(NULL, 0x08, 0xe12fff10);      /* BX  r0  -> Thumb    */
    m_w16(NULL, 0x10, 0x2105);          /* Thumb: MOV r1,#5    */
    m_w16(NULL, 0x12, 0x4710);          /* Thumb: BX r2 -> ARM */
    m_w32(NULL, 0x20, 0xe3a03009);      /* ARM:   MOV r3,#9    */

    arm_cpu_t c; arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS;
    for (int i = 0; i < 6; i++) arm_step(&c);

    CHECK(c.r[1] == 5, "r1=%u expect 5 (Thumb code ran)", c.r[1]);
    CHECK(c.r[3] == 9, "r3=%u expect 9 (returned to ARM)", c.r[3]);
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "T bit should be clear back in ARM state");
}

static void test_thumb_bl_pair(void) {
    /* BL is a 32-bit pair in Thumb. The halves combine into one 22-bit offset:
     * target = (PC_of_first + 4) + (offset << 1). With offset 2 that is
     * 4 + 4 = 8. LR must be the address after the pair, with the Thumb bit set
     * so the eventual BX LR returns to Thumb state. */
    uint16_t p[] = { 0xf000, 0xf802 };
    arm_cpu_t c; load_thumb(&c, p, 2, 2);
    CHECK(c.r[15] == 0x08, "pc=%08x expect 08", c.r[15]);
    CHECK(c.r[14] == 0x05, "lr=%08x expect 05 (return addr | Thumb bit)", c.r[14]);
}

static void test_thumb_extend_and_reverse(void) {
    /* ARMv6 Thumb extend group. Real Apple LLB reaches UXTB within a few
     * thousand instructions, so these are required, not optional. */
    uint16_t p[] = { 0x21ff,   /* MOV  r1,#0xff        */
                     0xb2ca,   /* UXTB r2,r1  -> 0xff  */
                     0xb24b,   /* SXTB r3,r1  -> -1    */
                     0xb289 }; /* UXTH r1,r1  -> 0xff  */
    arm_cpu_t c; load_thumb(&c, p, 4, 4);
    CHECK(c.r[2] == 0xff, "r2=%08x expect ff (UXTB)", c.r[2]);
    CHECK(c.r[3] == 0xffffffffu, "r3=%08x expect ffffffff (SXTB)", c.r[3]);
    CHECK(c.r[1] == 0xff, "r1=%08x expect ff (UXTH)", c.r[1]);
}

static void test_thumb_rev(void) {
    /* Build 0x11223344 a byte at a time, then REV it. */
    uint16_t p[] = { 0x2011, 0x0200, 0x3022, 0x0200,
                     0x3033, 0x0200, 0x3044, 0xba01 };
    arm_cpu_t c; load_thumb(&c, p, 8, 8);
    CHECK(c.r[0] == 0x11223344u, "r0=%08x expect 11223344", c.r[0]);
    CHECK(c.r[1] == 0x44332211u, "r1=%08x expect 44332211 (REV)", c.r[1]);
}

static void test_thumb_cps(void) {
    /* CPSID i then CPSIE i must set and clear the CPSR I bit. */
    uint16_t p[] = { 0xb672, 0xb662 };
    arm_cpu_t c; load_thumb(&c, p, 2, 1);
    CHECK((c.cpsr & ARM_CPSR_I) != 0, "CPSID should mask IRQs");
    arm_step(&c);
    CHECK((c.cpsr & ARM_CPSR_I) == 0, "CPSIE should unmask IRQs");
}

static void test_mmu_supersection(void) {
    /* Regression from booting real XNU: a type-2 descriptor with bit 18 set is
     * a 16 MB SUPERsection, taking its base from bits[31:24] and the offset
     * from va[23:0] — a different split from the 1 MB section. Treating one as
     * a section silently resolves the wrong physical address, and the kernel
     * read garbage where a valid pointer lived. */
    arm_cpu_t c;
    memset(g_ram, 0, sizeof g_ram);
    uint32_t super = 0x08000000u | (1u << 18) | (3u << 10) | 2u;
    for (unsigned i = 0; i < 16; i++)
        m_w32(NULL, 0x4000 + ((((0xc0000000u >> 20) + i)) << 2), super);
    arm_reset(&c, &g_bus);
    c.cp15.ttbr0 = 0x4000; c.cp15.dacr = 1u; c.cp15.sctlr |= ARM_SCTLR_M;

    uint32_t pa = 0;
    CHECK(arm_mmu_translate(&c, 0xc020e220u, ARM_ACCESS_READ, true, &pa) == 0,
          "supersection translation faulted");
    CHECK(pa == 0x0820e220u,
          "pa=%08x expect 0820e220 (supersection uses va[23:0])", pa);

    /* A plain section must still use the 1 MB split. */
    uint32_t sect = 0x08000000u | (3u << 10) | 2u;
    m_w32(NULL, 0x4000 + ((0xc0000000u >> 20) << 2), sect);
    CHECK(arm_mmu_translate(&c, 0xc0001234u, ARM_ACCESS_READ, true, &pa) == 0,
          "section faulted");
    CHECK(pa == 0x08001234u, "pa=%08x expect 08001234 (plain section)", pa);
}

static void test_thumb_blx_suffix_is_not_a_branch(void) {
    /* Regression from booting real XNU: 0xE000-0xE7FF is an unconditional
     * branch but 0xE800-0xEFFF is the SECOND half of a BLX pair, which returns
     * to ARM state. Treating the whole 0xExxx range as a branch sent BLX to a
     * garbage address, and the kernel executed a pointer table as code. */
    uint16_t p[] = { 0xf000, 0xe802 };
    arm_cpu_t c; load_thumb(&c, p, 2, 2);
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "BLX suffix must switch back to ARM state");
    CHECK((c.r[15] & 3u) == 0, "pc=%08x must be word-aligned after BLX", c.r[15]);
    CHECK(c.r[14] == 0x05, "lr=%08x expect 05 (return addr | Thumb bit)", c.r[14]);

    /* A plain unconditional Thumb branch must still branch. */
    uint16_t b[] = { 0xe000, 0x2007 };
    arm_cpu_t c2; load_thumb(&c2, b, 2, 2);
    CHECK((c2.cpsr & ARM_CPSR_T) != 0, "a plain branch must stay in Thumb");
}

static void test_user_bank_stm(void) {
    /* STM with the S bit transfers the USER bank whatever mode we are in —
     * how a kernel saves user context on exception entry. Real XNU uses
     * "STMIA sp,{r0-r14}^". */
    arm_cpu_t c; arm_reset(&c, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    arm_set_mode(&c, ARM_MODE_USR);
    c.r[13] = 0xaaaa0000u; c.r[14] = 0xbbbb0000u;
    arm_set_mode(&c, ARM_MODE_SVC);
    c.r[13] = 0x800; c.r[14] = 0xcccc0000u;
    m_w32(NULL, 0, 0xe8cd6000u);          /* STMIA sp,{r13,r14}^  (S bit set) */
    c.r[15] = 0;
    arm_step(&c);
    CHECK(m_r32(NULL, 0x800) == 0xaaaa0000u,
          "stored r13=%08x expect the USER bank aaaa0000", m_r32(NULL, 0x800));
    CHECK(m_r32(NULL, 0x804) == 0xbbbb0000u,
          "stored r14=%08x expect the USER bank bbbb0000", m_r32(NULL, 0x804));
    CHECK(c.r[14] == 0xcccc0000u, "the SVC bank must be untouched");
}

/* ------------------------------------------------ TTBCR.N / TTBR1 split ---
 * ARM ARM (ARMv6) B4.9.4 "Selecting between TTBR0 and TTBR1":
 *
 *   N == 0  -> every VA walks TTBR0; TTBR1 is not used at all.
 *   N  > 0  -> if VA[31:32-N] are all zero the walk starts at TTBR0, whose
 *              table is 2^(14-N) bytes based at TTBR0[31:14-N] and indexed by
 *              VA[31-N:20] (12-N index bits); otherwise it starts at TTBR1,
 *              which is ALWAYS a full 16 KB table indexed by VA[31:20].
 *
 * The tests below plant a decoy descriptor wherever a plausible mis-implementation
 * (unmasked base, wrong index width, inverted or off-by-one selector, TTBR0
 * fall-back) would land, so a wrong walker resolves to a recognisable PA rather
 * than merely faulting.
 */

/* Point the CPU at a TTBR0/TTBR1 pair with a given TTBCR.N, MMU on, domain 0 a
 * client so the AP bits are really checked. Clears RAM — plant descriptors
 * after calling this, not before. */
static void mmu_setup_split(arm_cpu_t *c, unsigned n,
                            uint32_t ttbr0, uint32_t ttbr1) {
    memset(g_ram, 0, sizeof g_ram);
    arm_reset(c, &g_bus);
    c->cp15.ttbr0  = ttbr0;
    c->cp15.ttbr1  = ttbr1;
    c->cp15.ttbcr  = n;
    c->cp15.dacr   = 1u;                      /* domain 0: client */
    c->cp15.sctlr |= ARM_SCTLR_M;             /* MMU on */
}

/* Plant a 1 MB section (AP=11, domain 0) at an explicit first-level index. The
 * index is spelled out instead of derived from the VA because which table and
 * which index the walker picks is precisely what is under test. The base and
 * offset are OR-ed exactly as the walker combines them, so a decoy written at an
 * unmasked base lands where a mis-masked walker would read. */
static void mmu_put_section(uint32_t table, unsigned index, uint32_t pa) {
    m_w32(NULL, table | (index << 2), (pa & 0xfff00000u) | (3u << 10) | 2u);
}

/* Privileged read translation; returns the PA, or 0xffffffff if it faulted. */
static uint32_t mmu_xlate(arm_cpu_t *c, uint32_t va) {
    uint32_t pa = 0;
    return arm_mmu_translate(c, va, ARM_ACCESS_READ, true, &pa) ? 0xffffffffu : pa;
}

static void test_mmu_ttbcr_n0_ignores_ttbr1(void) {
    /* N == 0 is the reset state and must keep behaving exactly as the walker did
     * before the split existed: a single 4096-entry TTBR0 table covering the
     * whole 4 GB, based at TTBR0[31:14], with TTBR1 never consulted. */
    arm_cpu_t c; mmu_setup_split(&c, 0, 0x4200u, 0x8000u); /* base masks to 0x4000 */
    mmu_put_section(0x4000, 0x001, 0x00400000u);   /* VA 0x00100000 */
    mmu_put_section(0x4000, 0xc00, 0x00200000u);   /* VA 0xc0000000 */
    mmu_put_section(0x4000, 0xfff, 0x00500000u);   /* VA 0xfff00000, last entry */
    mmu_put_section(0x4200, 0x001, 0x00900000u);   /* decoy: unmasked TTBR0 base */
    mmu_put_section(0x8000, 0xc00, 0x00900000u);   /* decoy: TTBR1 must be ignored */
    mmu_put_section(0x8000, 0xfff, 0x00900000u);   /* decoy: TTBR1 must be ignored */

    uint32_t pa = mmu_xlate(&c, 0x00100abcu);
    CHECK(pa == 0x00400abcu, "pa=%08x expect 00400abc (low VA via TTBR0)", pa);
    pa = mmu_xlate(&c, 0xc0001234u);
    CHECK(pa == 0x00201234u, "pa=%08x expect 00201234 (N=0: high VA still TTBR0)", pa);
    pa = mmu_xlate(&c, 0xfff00010u);
    CHECK(pa == 0x00500010u, "pa=%08x expect 00500010 (N=0 table is 4096 entries)", pa);
}

static void test_mmu_ttbcr_n2_low_va_uses_shrunk_ttbr0(void) {
    /* N == 2 is what XNU-ARM runs with. TTBR0's table shrinks to 2^(14-2) = 4 KB
     * / 1024 entries, its base is TTBR0[31:12] (the low 12 bits of the register
     * are not part of the address), and it is indexed by VA[29:20] — ten bits. */
    arm_cpu_t c; mmu_setup_split(&c, 2, 0x5400u, 0x8000u); /* base masks to 0x5000 */
    mmu_put_section(0x5000, 0x001, 0x00700000u);   /* VA 0x00100000 */
    mmu_put_section(0x5000, 0x3ff, 0x00800000u);   /* VA 0x3ff00000, last entry */
    mmu_put_section(0x5400, 0x001, 0x00900000u);   /* decoy: unmasked TTBR0 base */

    uint32_t pa = mmu_xlate(&c, 0x00100abcu);
    CHECK(pa == 0x00700abcu,
          "pa=%08x expect 00700abc (N=2 TTBR0 base masks to a 4 KB boundary)", pa);
    pa = mmu_xlate(&c, 0x3ff00abcu);
    CHECK(pa == 0x00800abcu,
          "pa=%08x expect 00800abc (last entry of the 1024-entry TTBR0 table)", pa);
}

static void test_mmu_ttbcr_n2_kernel_va_uses_ttbr1(void) {
    /* With N == 2 everything at or above 0x40000000 walks TTBR1: all of kernel
     * text at 0xc0000000 and the 0xffff0000 exception-vector page. TTBR1's table
     * is a full 16 KB indexed by VA[31:20] regardless of N. */
    arm_cpu_t c; mmu_setup_split(&c, 2, 0x5000u, 0x8234u); /* base masks to 0x8000 */
    mmu_put_section(0x8000, 0xc00, 0x00200000u);   /* kernel text  0xc0000000 */
    mmu_put_section(0x8000, 0xfff, 0x00300000u);   /* vector page  0xffff0000 */
    mmu_put_section(0x8234, 0xc00, 0x00900000u);   /* decoy: unmasked TTBR1 base */
    /* 0xd0000000 has no TTBR1 entry. Give the TTBR0 table an entry at the index
     * a truncated walk would use (0xd00 & 0x3ff) so a fall-back is visible. */
    mmu_put_section(0x5000, 0x100, 0x00a00000u);

    uint32_t pa = mmu_xlate(&c, 0xc0000abcu);
    CHECK(pa == 0x00200abcu, "pa=%08x expect 00200abc (kernel text via TTBR1)", pa);
    /* 0xffff000c lies 0xf000c into the 1 MB section based at 0xfff00000. */
    pa = mmu_xlate(&c, 0xffff000cu);
    CHECK(pa == 0x003f000cu, "pa=%08x expect 003f000c (vector page via TTBR1)", pa);

    uint32_t p = 0;
    uint32_t f = arm_mmu_translate(&c, 0xd0000000u, ARM_ACCESS_READ, true, &p);
    CHECK((f & 0xf) == ARM_FSR_SECTION_TRANSLATION,
          "fsr=%x expect a translation fault: a TTBR1 miss must not fall back "
          "to TTBR0 (got pa=%08x)", f, p);
}

static void test_mmu_ttbcr_n2_split_boundary(void) {
    /* The selector with N == 2 is VA[31:30]: 0x3ff00000 is the last VA that
     * still walks TTBR0 and 0x40000000 is the first that crosses to TTBR1. */
    arm_cpu_t c; mmu_setup_split(&c, 2, 0x5000u, 0x8000u);
    mmu_put_section(0x5000, 0x3ff, 0x00100000u);   /* TTBR0 side of the line */
    mmu_put_section(0x8000, 0x400, 0x00200000u);   /* TTBR1 side of the line */
    /* 0x40000000's index truncated to ten bits is 0 — where the walk would land
     * if the index were shrunk but the table not switched. */
    mmu_put_section(0x5000, 0x000, 0x00900000u);
    /* and TTBR1 at the TTBR0-side index, in case the selector is inverted. */
    mmu_put_section(0x8000, 0x3ff, 0x00a00000u);

    uint32_t pa = mmu_xlate(&c, 0x3ff00abcu);
    CHECK(pa == 0x00100abcu,
          "pa=%08x expect 00100abc (0x3ff00000 is still TTBR0 with N=2)", pa);
    pa = mmu_xlate(&c, 0x40000abcu);
    CHECK(pa == 0x00200abcu,
          "pa=%08x expect 00200abc (0x40000000 is the first TTBR1 VA with N=2)", pa);
}

static void test_mmu_kernel_mapping_survives_ttbr0_pmap_switch(void) {
    /* THE regression the split exists to fix. XNU runs with N == 2, keeps the
     * kernel in TTBR1 and the *current user* pmap in TTBR0, and rewrites TTBR0
     * on every context switch (pmap_switch -> set_mmu_ttb). A walker that always
     * used TTBR0 lost kernel text and the 0xffff0000 vector page the instant
     * that happened, and the CPU stormed on prefetch aborts at 0xffff000c. */
    const uint32_t pmap_a = 0x5000u, pmap_b = 0x6000u, kernel = 0x8000u;
    arm_cpu_t c; mmu_setup_split(&c, 2, pmap_a, kernel);
    mmu_put_section(kernel, 0xc00, 0x00200000u);   /* kernel text  0xc0000000 */
    mmu_put_section(kernel, 0xfff, 0x00300000u);   /* vector page  0xffff0000 */
    mmu_put_section(pmap_a, 0x001, 0x00500000u);   /* user 0x00100000 under A */
    mmu_put_section(pmap_b, 0x001, 0x00600000u);   /* user 0x00100000 under B */

    uint32_t text_a = mmu_xlate(&c, 0xc0001000u);
    uint32_t vec_a  = mmu_xlate(&c, 0xffff000cu);
    uint32_t usr_a  = mmu_xlate(&c, 0x00100000u);
    CHECK(text_a == 0x00201000u, "pa=%08x expect 00201000 (kernel text, pmap A)", text_a);
    CHECK(vec_a  == 0x003f000cu, "pa=%08x expect 003f000c (vector page, pmap A)", vec_a);
    CHECK(usr_a  == 0x00500000u, "pa=%08x expect 00500000 (user VA via pmap A)", usr_a);

    c.cp15.ttbr0 = pmap_b;                          /* pmap_switch() to another task */

    uint32_t usr_b  = mmu_xlate(&c, 0x00100000u);
    CHECK(usr_b == 0x00600000u,
          "pa=%08x expect 00600000 — the TTBR0 switch must take effect for user VAs",
          usr_b);
    uint32_t text_b = mmu_xlate(&c, 0xc0001000u);
    uint32_t vec_b  = mmu_xlate(&c, 0xffff000cu);
    CHECK(text_b == text_a,
          "pa=%08x expect %08x — kernel text must survive a TTBR0 pmap switch",
          text_b, text_a);
    CHECK(vec_b == vec_a,
          "pa=%08x expect %08x — the vector page must survive a TTBR0 pmap switch",
          vec_b, vec_a);
}

static void test_mmu_ttbcr_n1_table_geometry(void) {
    /* N == 1: TTBR0's table is 2^(14-1) = 8 KB / 2048 entries based at
     * TTBR0[31:13] and indexed by VA[30:20]; the split falls at 0x80000000. */
    arm_cpu_t c; mmu_setup_split(&c, 1, 0x6800u, 0x8000u); /* base masks to 0x6000 */
    mmu_put_section(0x6000, 0x001, 0x00300000u);   /* VA 0x00100000 */
    mmu_put_section(0x6000, 0x7ff, 0x00100000u);   /* VA 0x7ff00000, last entry */
    mmu_put_section(0x8000, 0x800, 0x00200000u);   /* VA 0x80000000, first TTBR1 VA */
    mmu_put_section(0x6800, 0x001, 0x00900000u);   /* decoy: unmasked TTBR0 base */
    mmu_put_section(0x6000, 0x000, 0x00a00000u);   /* decoy: index shrunk, table not switched */

    uint32_t pa = mmu_xlate(&c, 0x00100abcu);
    CHECK(pa == 0x00300abcu,
          "pa=%08x expect 00300abc (N=1 TTBR0 base masks to an 8 KB boundary)", pa);
    pa = mmu_xlate(&c, 0x7ff00abcu);
    CHECK(pa == 0x00100abcu,
          "pa=%08x expect 00100abc (0x7ff00000 is the last TTBR0 VA with N=1)", pa);
    pa = mmu_xlate(&c, 0x80000abcu);
    CHECK(pa == 0x00200abcu,
          "pa=%08x expect 00200abc (0x80000000 is the first TTBR1 VA with N=1)", pa);
}

static void test_mmu_ttbcr_n3_table_geometry(void) {
    /* N == 3: TTBR0's table is 2^(14-3) = 2 KB / 512 entries based at
     * TTBR0[31:11] and indexed by VA[28:20]; the split falls at 0x20000000.
     * TTBR1's table stays a full 16 KB indexed by VA[31:20] whatever N is. */
    arm_cpu_t c; mmu_setup_split(&c, 3, 0x6400u, 0x8000u); /* base masks to 0x6000 */
    mmu_put_section(0x6000, 0x001, 0x00300000u);   /* VA 0x00100000 */
    mmu_put_section(0x6000, 0x1ff, 0x00100000u);   /* VA 0x1ff00000, last entry */
    mmu_put_section(0x8000, 0x200, 0x00200000u);   /* VA 0x20000000, first TTBR1 VA */
    mmu_put_section(0x8000, 0xfff, 0x00400000u);   /* VA 0xfff00000 via TTBR1 */
    mmu_put_section(0x6400, 0x001, 0x00900000u);   /* decoy: unmasked TTBR0 base */
    mmu_put_section(0x6000, 0x000, 0x00a00000u);   /* decoy: index shrunk, table not switched */

    uint32_t pa = mmu_xlate(&c, 0x00100abcu);
    CHECK(pa == 0x00300abcu,
          "pa=%08x expect 00300abc (N=3 TTBR0 base masks to a 2 KB boundary)", pa);
    pa = mmu_xlate(&c, 0x1ff00abcu);
    CHECK(pa == 0x00100abcu,
          "pa=%08x expect 00100abc (0x1ff00000 is the last TTBR0 VA with N=3)", pa);
    pa = mmu_xlate(&c, 0x20000abcu);
    CHECK(pa == 0x00200abcu,
          "pa=%08x expect 00200abc (0x20000000 is the first TTBR1 VA with N=3)", pa);
    pa = mmu_xlate(&c, 0xffff000cu);
    CHECK(pa == 0x004f000cu,
          "pa=%08x expect 004f000c (TTBR1 is a full 16 KB table for every N)", pa);
}

/* ------------------------------------------------- DFSR / IFSR completeness -
 * The fault status registers are the whole conversation between this CPU and
 * XNU's abort handlers, and a single missing bit in them cost a full diagnosis
 * cycle (DFSR.WnR, commit 85c4653). These tests pin down every field the
 * kernel is known to read.
 *
 * What xnu-1357.5.30 actually reads, from the shipped kernel:
 *   _fleh_dataabt (0xc0068338)  SUB lr,lr,#8 ; MRC p15,0,r5,c5,c0,0 (DFSR)
 *                                            ; MRC p15,0,r6,c6,c0,0 (DFAR)
 *   _fleh_prefabt (0xc006828c)  SUB lr,lr,#4 ; MRC p15,0,r1,c6,c0,2 (IFAR)
 *                                            ; MRC p15,0,r5,c5,c0,1 (IFSR)
 *   _sleh_abort   (0xc006c538)  status = fsr & 0x40f, and for a data abort
 *                               TST fsr,#0x800 — DFSR.WnR — selecting
 *                               fault_type 3 (read|write) instead of 1 (read).
 */

/* Two 1 MB sections through one L1 table at 0x4000: an identity map for the
 * low megabyte (so the instruction fetch itself always succeeds) and a test
 * mapping at 0x80000000 with the caller's AP. 0x80100000 is deliberately left
 * without a descriptor. `mode` is the mode to execute in. */
static void fsr_setup(arm_cpu_t *c, const uint32_t *prog, size_t words,
                      unsigned ap, uint32_t mode) {
    memset(g_ram, 0, sizeof g_ram);
    for (size_t i = 0; i < words; i++) m_w32(NULL, (uint32_t)(i * 4), prog[i]);
    m_w32(NULL, 0x4000 + (0x000u << 2), 0x00000000u | (3u << 10) | 2u);
    m_w32(NULL, 0x4000 + (0x800u << 2), 0x00000000u | (ap  << 10) | 2u);
    arm_reset(c, &g_bus);
    c->cp15.ttbr0 = 0x4000; c->cp15.dacr = 1u; c->cp15.sctlr |= ARM_SCTLR_M;
    arm_set_mode(c, mode);
    /* Reset leaves CPSR.A set. Clear it so the assertions below prove that
     * *exception entry* sets it, rather than passing on a leftover. */
    c->cpsr &= ~ARM_CPSR_A;
    c->r[15] = 0;
}

static void test_dfsr_wnr_write_vs_read(void) {
    /* At the translator itself: the SAME fault, reached by a read and by a
     * write, must differ in exactly one bit. AP=00 is "no access", so both
     * directions fault with the same status and the only difference is WnR. */
    arm_cpu_t c; mmu_setup_section(&c, 0x80000000u, 0x00200000u, 0, 0);
    uint32_t pa = 0;
    uint32_t rd = arm_mmu_translate(&c, 0x80000abcu, ARM_ACCESS_READ, true, &pa);
    uint32_t wr = arm_mmu_translate(&c, 0x80000abcu, ARM_ACCESS_WRITE, true, &pa);
    CHECK((rd & 0xfu) == ARM_FSR_SECTION_PERMISSION,
          "fsr=%08x expect a section permission fault on read", rd);
    CHECK((wr & 0xfu) == ARM_FSR_SECTION_PERMISSION,
          "fsr=%08x expect a section permission fault on write", wr);
    CHECK((rd & (1u << 11)) == 0, "fsr=%08x: WnR must be CLEAR for a read", rd);
    CHECK((wr & (1u << 11)) != 0, "fsr=%08x: WnR must be SET for a write", wr);
    CHECK((rd ^ wr) == (1u << 11),
          "read fsr %08x and write fsr %08x must differ in bit 11 alone", rd, wr);

    /* WnR is orthogonal to the status code: a translation fault carries it too,
     * and so does an unprivileged access. */
    uint32_t tw = arm_mmu_translate(&c, 0x90000000u, ARM_ACCESS_WRITE, false, &pa);
    CHECK((tw & 0xfu) == ARM_FSR_SECTION_TRANSLATION,
          "fsr=%08x expect a translation fault", tw);
    CHECK((tw & (1u << 11)) != 0,
          "fsr=%08x: WnR must be SET on a write translation fault too", tw);

    /* And no field above bit 11 is ever invented: ExT (bit 12) and the
     * extended status bit (bit 10) have no source in this machine. */
    CHECK((wr & ~0x8ffu) == 0,
          "fsr=%08x has bits set outside status/domain/WnR", wr);
}

static void test_data_abort_write_sets_dfsr_wnr(void) {
    /* End to end, the way XNU sees it: an unprivileged store to a
     * privileged-only page. This is the exact shape of the copyout that
     * livelocked ~2.8 million times with WnR missing. */
    uint32_t p[] = { 0xe3a01102 /*MOV r1,#0x80000000*/,
                     0xe5812000 /*STR r2,[r1]        */ };
    arm_cpu_t c; fsr_setup(&c, p, 2, 1 /*AP=01: privileged RW only*/, ARM_MODE_USR);
    arm_step(&c);
    uint32_t before = c.cpsr;
    arm_step(&c);

    CHECK(c.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (data abort)", c.r[15]);
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_SECTION_PERMISSION,
          "dfsr=%08x expect a section permission fault", c.cp15.dfsr);
    CHECK((c.cp15.dfsr & (1u << 11)) != 0,
          "dfsr=%08x: bit 11 (WnR) must be SET for a store", c.cp15.dfsr);
    CHECK(c.cp15.dfar == 0x80000000u, "dfar=%08x expect 80000000", c.cp15.dfar);

    /* Abort-mode entry state, checked against what fleh_dataabt assumes:
     * it does SUB lr,lr,#8 to recover the faulting instruction's address, and
     * then LDRs the instruction word from it to classify the access. */
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_ABT,
          "mode=%02x expect ABT", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.r[14] == 4u + 8u, "lr=%08x expect 0c (faulting insn 0x04 + 8)", c.r[14]);
    CHECK(c.spsr[ARM_BANK_ABT] == before,
          "spsr=%08x expect the pre-fault cpsr %08x", c.spsr[ARM_BANK_ABT], before);
    CHECK((c.cpsr & ARM_CPSR_I) != 0, "IRQs must be masked on abort entry");
    CHECK((c.cpsr & ARM_CPSR_A) != 0,
          "cpsr=%08x: ARMv6 sets CPSR.A on data-abort entry", c.cpsr);
    CHECK((c.cpsr & ARM_CPSR_T) == 0, "exceptions must enter in ARM state");

    /* The same page, same instruction shape, read instead of write. */
    uint32_t q[] = { 0xe3a01102 /*MOV r1,#0x80000000*/,
                     0xe5912000 /*LDR r2,[r1]        */ };
    arm_cpu_t d; fsr_setup(&d, q, 2, 1, ARM_MODE_USR);
    arm_step(&d); arm_step(&d);
    CHECK(d.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (data abort)", d.r[15]);
    CHECK((d.cp15.dfsr & 0xfu) == ARM_FSR_SECTION_PERMISSION,
          "dfsr=%08x expect a section permission fault", d.cp15.dfsr);
    CHECK((d.cp15.dfsr & (1u << 11)) == 0,
          "dfsr=%08x: bit 11 (WnR) must be CLEAR for a load", d.cp15.dfsr);
}

static void test_prefetch_abort_never_sets_wnr(void) {
    /* IFSR has no WnR field. The fetch path must translate as a read, and it
     * must leave DFSR/DFAR — which belong to the data side — untouched, because
     * a prefetch abort taken while a data fault is still recorded would
     * otherwise rewrite the kernel's view of the earlier fault. */
    uint32_t p[] = { 0xe3a00102 /*MOV r0,#0x80000000*/,
                     0xe1a0f000 /*MOV pc,r0         */ };
    arm_cpu_t c; fsr_setup(&c, p, 2, 3, ARM_MODE_SYS);
    c.cp15.dfsr = 0xdeadbeefu; c.cp15.dfar = 0xcafebabeu;   /* sentinels */
    arm_step(&c);
    /* 0x80100000 is not an encodable ARM immediate, so aim the branch there by
     * hand: it is the megabyte just past the mapping, with no descriptor. */
    c.r[0] = 0x80100000u;
    arm_step(&c);                            /* MOV pc,r0 */
    CHECK(c.r[15] == 0x80100000u, "pc=%08x expect 80100000 before the fetch", c.r[15]);
    uint32_t before = c.cpsr;
    arm_step(&c);                            /* the fetch aborts */

    CHECK(c.r[15] == ARM_VEC_PREFETCH, "pc=%08x expect 0c (prefetch abort)", c.r[15]);
    CHECK((c.cp15.ifsr & 0xfu) == ARM_FSR_SECTION_TRANSLATION,
          "ifsr=%08x expect a section translation fault", c.cp15.ifsr);
    CHECK((c.cp15.ifsr & (1u << 11)) == 0,
          "ifsr=%08x: the instruction-fetch path must never set bit 11 — "
          "IFSR has no WnR field", c.cp15.ifsr);
    CHECK(c.cp15.ifar == 0x80100000u, "ifar=%08x expect 80100000", c.cp15.ifar);
    CHECK(c.cp15.dfsr == 0xdeadbeefu,
          "dfsr=%08x: a prefetch abort must not touch the data-side FSR", c.cp15.dfsr);
    CHECK(c.cp15.dfar == 0xcafebabeu,
          "dfar=%08x: a prefetch abort must not touch the data-side FAR", c.cp15.dfar);
    /* fleh_prefabt does SUB lr,lr,#4 to recover the faulting address. */
    CHECK(c.r[14] == 0x80100000u + 4u, "lr=%08x expect 80100004", c.r[14]);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_ABT,
          "mode=%02x expect ABT", c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK(c.spsr[ARM_BANK_ABT] == before,
          "spsr=%08x expect the pre-fault cpsr %08x", c.spsr[ARM_BANK_ABT], before);
    CHECK((c.cpsr & ARM_CPSR_A) != 0,
          "cpsr=%08x: ARMv6 sets CPSR.A on prefetch-abort entry", c.cpsr);
}

static void test_dfar_is_the_faulting_word_of_a_block_transfer(void) {
    /* An LDM that runs off the end of a mapped page must report the address of
     * the word that actually faulted, not the base of the transfer. sleh_abort
     * page-aligns DFAR (fp & 0xfffff000) and hands it to arm_fast_fault, so
     * reporting the base would map in the page the transfer already had and
     * re-execute the same instruction forever. */
    uint32_t p[] = { 0xe59f1008 /*LDR  r1,[pc,#8] -> the literal at 0x10 */,
                     0xe8910007 /*LDMIA r1,{r0,r1,r2}                    */,
                     0xeafffffe /*B .                                    */,
                     0x00000000 /*(pad)                                  */,
                     0x800ffff8 /*literal: 8 bytes before the page end   */ };
    arm_cpu_t c; fsr_setup(&c, p, 5, 3, ARM_MODE_SYS);
    c.r[0] = 0x11111111u; c.r[2] = 0x33333333u;
    arm_step(&c);
    CHECK(c.r[1] == 0x800ffff8u, "r1=%08x expect 800ffff8", c.r[1]);
    arm_step(&c);

    CHECK(c.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (data abort)", c.r[15]);
    CHECK(c.cp15.dfar == 0x80100000u,
          "dfar=%08x expect 80100000 — the third word of the LDM, not the base "
          "0x800ffff8", c.cp15.dfar);
    CHECK((c.cp15.dfsr & (1u << 11)) == 0,
          "dfsr=%08x: an LDM is a read, WnR must be clear", c.cp15.dfsr);
    /* Base Restored Abort Model: nothing in the register file moved. */
    CHECK(c.r[0] == 0x11111111u, "r0=%08x expect 11111111 (base-restored)", c.r[0]);
    CHECK(c.r[1] == 0x800ffff8u, "r1=%08x expect 800ffff8 (base-restored)", c.r[1]);
    CHECK(c.r[2] == 0x33333333u, "r2=%08x expect 33333333 (base-restored)", c.r[2]);

    /* The mirror-image STM must set WnR and report the same address. */
    uint32_t q[] = { 0xe59f1008, 0xe8810007 /*STMIA r1,{r0,r1,r2}*/,
                     0xeafffffe, 0x00000000, 0x800ffff8 };
    arm_cpu_t d; fsr_setup(&d, q, 5, 3, ARM_MODE_SYS);
    arm_step(&d); arm_step(&d);
    CHECK(d.cp15.dfar == 0x80100000u,
          "dfar=%08x expect 80100000 for the storing form too", d.cp15.dfar);
    CHECK((d.cp15.dfsr & (1u << 11)) != 0,
          "dfsr=%08x: an STM is a write, WnR must be set", d.cp15.dfsr);
}

static void test_cps_is_a_nop_in_user_mode(void) {
    /* "CPS is a no-op if executed in User mode" (ARM ARM A4.1.16). Executing it
     * anyway lets a user program run "CPS #0x13" and continue in SVC with its
     * own registers. Nothing in a kernel-only boot can expose this, because XNU
     * only ever reaches CPS from a privileged mode. */
    uint32_t p[] = { 0xf1020013 /*CPS #0x13 (to SVC)*/,
                     0xf1080080 /*CPSIE i           */,
                     0xe3a00007 /*MOV r0,#7         */ };
    arm_cpu_t c; arm_reset(&c, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    for (unsigned i = 0; i < 3; i++) m_w32(NULL, i * 4, p[i]);
    arm_set_mode(&c, ARM_MODE_USR);
    c.cpsr |= ARM_CPSR_I;
    c.r[15] = 0;
    for (int i = 0; i < 3; i++) arm_step(&c);

    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR,
          "mode=%02x expect USR — CPS must not change mode from User mode",
          c.cpsr & ARM_CPSR_MODE_MASK);
    CHECK((c.cpsr & ARM_CPSR_I) != 0,
          "cpsr=%08x: CPSIE from User mode must not unmask interrupts", c.cpsr);
    CHECK(c.r[0] == 7, "r0=%u expect 7 — CPS is a no-op, not a trap", c.r[0]);

    /* From a privileged mode it must still work, or the kernel cannot mask
     * interrupts at all. */
    arm_cpu_t d; arm_reset(&d, &g_bus);
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, 0, 0xf1020013u);
    arm_set_mode(&d, ARM_MODE_SYS);
    d.r[15] = 0;
    arm_step(&d);
    CHECK((d.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC,
          "mode=%02x expect SVC — privileged CPS must still change mode",
          d.cpsr & ARM_CPSR_MODE_MASK);
}

/* --- page-crossing unaligned accesses, and XN --------------------------- */

/*
 * Two 4 KB small pages behind one coarse table, at VA 0x80000000 and
 * 0x80001000, whose physical frames the caller chooses — and chooses to be far
 * apart, because that is the whole point: a walker that translates only the
 * base of a straddling access reads or writes across the *physical* boundary,
 * which is silently the wrong memory whenever the frames are not adjacent.
 * A frame of 0 leaves that page with no second-level descriptor at all.
 * `xn1` sets execute-never (bit 0 of a small-page descriptor) on the second.
 *
 * The low megabyte is identity-mapped by a section so the instruction fetch
 * always succeeds, and SCTLR.XP is set because that is the configuration XNU
 * runs in and the only one in which XN exists.
 */
static void split_setup(arm_cpu_t *c, const uint32_t *prog, size_t words,
                        uint32_t pa0, uint32_t pa1, bool xn1) {
    const uint32_t l1 = 0x4000, l2 = 0x5000;
    memset(g_ram, 0, sizeof g_ram);
    for (size_t i = 0; i < words; i++) m_w32(NULL, (uint32_t)(i * 4), prog[i]);
    m_w32(NULL, l1 + (0x000u << 2), 0x00000000u | (3u << 10) | 2u);   /* identity */
    m_w32(NULL, l1 + (0x800u << 2), (l2 & 0xfffffc00u) | 1u);         /* coarse   */
    if (pa0) m_w32(NULL, l2 + (0u << 2), (pa0 & 0xfffff000u) | (3u << 4) | 2u);
    if (pa1) m_w32(NULL, l2 + (1u << 2),
                   (pa1 & 0xfffff000u) | (3u << 4) | 2u | (xn1 ? 1u : 0u));
    arm_reset(c, &g_bus);
    c->cp15.ttbr0  = l1;
    c->cp15.dacr   = 1u;                       /* domain 0: client */
    c->cp15.sctlr |= ARM_SCTLR_M | ARM_SCTLR_XP;
    c->cpsr = (c->cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c->r[15] = 0;
}

static void test_unaligned_access_spanning_two_pages(void) {
    /* VA 0x80000ffe..0x80001001: two bytes in the frame at PA 0x30000 and two
     * in the frame at PA 0x60000. Translating the base once and doing a single
     * 32-bit bus read at 0x30ffe picks up 0x31000/0x31001 for the top half —
     * memory belonging to whatever else happens to sit after that frame, which
     * is why a decoy is planted there: it returned 0x8899bbaa before the fix. */
    uint32_t p[] = { 0xe59f1008 /*LDR r1,[pc,#8] -> literal at 0x10*/,
                     0xe5910000 /*LDR r0,[r1]                      */,
                     0xeafffffe /*B .                              */,
                     0x00000000,
                     0x80000ffeu };
    arm_cpu_t c; split_setup(&c, p, 5, 0x30000u, 0x60000u, false);
    m_w8(NULL, 0x30ffeu, 0xaa); m_w8(NULL, 0x30fffu, 0xbb);
    m_w8(NULL, 0x60000u, 0xcc); m_w8(NULL, 0x60001u, 0xdd);
    m_w8(NULL, 0x31000u, 0x99); m_w8(NULL, 0x31001u, 0x88);  /* the decoy */
    arm_step(&c); arm_step(&c);
    CHECK(c.r[0] == 0xddccbbaau,
          "r0=%08x expect ddccbbaa — the two halves come from PA 0x30ffe and "
          "PA 0x60000, not from one run of bytes at 0x30ffe", c.r[0]);

    /* The storing mirror: each half must land in its own frame, and nothing may
     * be written past the end of the first one. */
    uint32_t q[] = { 0xe59f1008, 0xe5810000 /*STR r0,[r1]*/, 0xeafffffe,
                     0x00000000, 0x80000ffeu };
    arm_cpu_t d; split_setup(&d, q, 5, 0x30000u, 0x60000u, false);
    arm_step(&d);
    d.r[0] = 0x11223344u;
    arm_step(&d);
    CHECK(m_r8(NULL, 0x30ffeu) == 0x44 && m_r8(NULL, 0x30fffu) == 0x33,
          "%02x %02x expect 44 33 in the first frame",
          m_r8(NULL, 0x30ffeu), m_r8(NULL, 0x30fffu));
    CHECK(m_r8(NULL, 0x60000u) == 0x22 && m_r8(NULL, 0x60001u) == 0x11,
          "%02x %02x expect 22 11 in the SECOND frame",
          m_r8(NULL, 0x60000u), m_r8(NULL, 0x60001u));
    CHECK(m_r8(NULL, 0x31000u) == 0 && m_r8(NULL, 0x31001u) == 0,
          "the store must not run past the end of the first physical frame");

    /* A halfword straddling the same boundary, and — the other side of the
     * test — a word ending exactly ON the boundary, which does NOT cross and
     * must still be served by a single whole-word bus access. */
    uint32_t h[] = { 0xe59f1008, 0xe1d100b0 /*LDRH r0,[r1]*/, 0xeafffffe,
                     0x00000000, 0x80000fffu };
    arm_cpu_t e; split_setup(&e, h, 5, 0x30000u, 0x60000u, false);
    m_w8(NULL, 0x30fffu, 0xbb); m_w8(NULL, 0x60000u, 0xcc);
    arm_step(&e); arm_step(&e);
    CHECK(e.r[0] == 0xccbbu, "r0=%08x expect ccbb for a straddling LDRH", e.r[0]);

    uint32_t w[] = { 0xe59f1008, 0xe5910000, 0xeafffffe, 0x00000000, 0x80000ffcu };
    arm_cpu_t g; split_setup(&g, w, 5, 0x30000u, 0x60000u, false);
    m_w32(NULL, 0x30ffcu, 0x12345678u);
    arm_step(&g); arm_step(&g);
    CHECK(g.r[0] == 0x12345678u,
          "r0=%08x expect 12345678 — 0xffc..0xfff is the last word wholly inside "
          "the page and must not be split", g.r[0]);
}

static void test_unaligned_access_faulting_on_the_second_page(void) {
    /* The half that faults is the one that must be reported. sleh_abort page-
     * aligns DFAR and repairs that page, so a fault on the second page reported
     * against the base address maps in the page the access already had and
     * re-executes the same instruction forever. */
    uint32_t p[] = { 0xe59f1008, 0xe5910000 /*LDR r0,[r1]*/, 0xeafffffe,
                     0x00000000, 0x80000ffeu };
    arm_cpu_t c; split_setup(&c, p, 5, 0x30000u, 0u /*second page absent*/, false);
    arm_step(&c);
    c.r[0] = 0xdeadbeefu;
    arm_step(&c);
    CHECK(c.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (data abort)", c.r[15]);
    CHECK((c.cp15.dfsr & 0xfu) == ARM_FSR_PAGE_TRANSLATION,
          "dfsr=%08x expect a page translation fault", c.cp15.dfsr);
    CHECK((c.cp15.dfsr & (1u << 11)) == 0, "dfsr=%08x: a load must not set WnR",
          c.cp15.dfsr);
    CHECK(c.cp15.dfar == 0x80001000u,
          "dfar=%08x expect 80001000 — the first byte that lies in the SECOND "
          "page, not the base 0x80000ffe", c.cp15.dfar);
    CHECK(c.r[0] == 0xdeadbeefu,
          "r0=%08x: base-restored abort model — the destination must not move",
          c.r[0]);
    CHECK(c.r[1] == 0x80000ffeu, "r1=%08x expect the base unchanged", c.r[1]);

    /* A store that faults on its second page. DFAR and WnR as above — and the
     * bytes that landed in the first page STAY there. Hardware issues the two
     * halves as separate bus transactions and cannot retract one that has
     * completed; the architecture restores the base *register*, not memory.
     * Re-execution after the handler maps the page rewrites the same bytes. */
    uint32_t q[] = { 0xe59f1008, 0xe5810000 /*STR r0,[r1]*/, 0xeafffffe,
                     0x00000000, 0x80000ffeu };
    arm_cpu_t d; split_setup(&d, q, 5, 0x30000u, 0u, false);
    arm_step(&d);
    d.r[0] = 0x11223344u;
    arm_step(&d);
    CHECK(d.r[15] == ARM_VEC_DATA_ABORT, "pc=%08x expect 10 (data abort)", d.r[15]);
    CHECK(d.cp15.dfar == 0x80001000u,
          "dfar=%08x expect 80001000 for the storing form too", d.cp15.dfar);
    CHECK((d.cp15.dfsr & (1u << 11)) != 0,
          "dfsr=%08x: a store must set WnR", d.cp15.dfsr);
    CHECK(m_r8(NULL, 0x30ffeu) == 0x44 && m_r8(NULL, 0x30fffu) == 0x33,
          "%02x %02x expect 44 33: the first page's bytes are already committed "
          "when the second page aborts", m_r8(NULL, 0x30ffeu), m_r8(NULL, 0x30fffu));
    CHECK(d.r[1] == 0x80000ffeu, "r1=%08x expect the base unchanged", d.r[1]);

    /* And when it is the FIRST page that is missing, the base is the right
     * answer and nothing is written anywhere. */
    arm_cpu_t e; split_setup(&e, q, 5, 0u, 0x60000u, false);
    arm_step(&e);
    e.r[0] = 0x11223344u;
    arm_step(&e);
    CHECK(e.cp15.dfar == 0x80000ffeu,
          "dfar=%08x expect 80000ffe when the first page is the bad one",
          e.cp15.dfar);
    CHECK(m_r8(NULL, 0x60000u) == 0 && m_r8(NULL, 0x60001u) == 0,
          "a fault on the first half must not let the second half through");
}

/*
 * XN — execute never.
 *
 * Confirmed live in xnu-1357.5.30 before being implemented: _nx_enabled
 * (0xc020e1b8) is a __DATA global initialised to 1 that nothing in the image
 * writes, and Thumb _pmap_enter (0xc006056c) builds its small-page template as
 * "pa | 2" but reaches an ORR that makes it "pa | 3" — XN set — whenever the
 * mapping is not VM_PROT_EXECUTE and both _nx_enabled and the per-pmap flag at
 * pmap+0x420 are non-zero. _pmap_disable_NX (0xc005fe44) is five instructions
 * that store 0 to that flag, and it is the only opt-out.
 */
static void test_xn_blocks_fetch_from_a_small_page(void) {
    uint32_t p[] = { 0xe59f0008 /*LDR r0,[pc,#8] -> literal at 0x10*/,
                     0xe1a0f000 /*MOV pc,r0                        */,
                     0xeafffffe /*B .                              */,
                     0x00000000,
                     0x80001000u };

    /* The permitted case first, so the fault below is known to be the XN bit
     * and not the mapping being broken: same page, XN clear, executes. */
    arm_cpu_t c; split_setup(&c, p, 5, 0x30000u, 0x60000u, false);
    m_w32(NULL, 0x60000u, 0xe3a02007u);             /* MOV r2,#7 at the target */
    arm_step(&c); arm_step(&c); arm_step(&c);
    CHECK(c.r[2] == 7, "r2=%u expect 7 — a page without XN must execute", c.r[2]);

    /* The same mapping with XN set. */
    arm_cpu_t d; split_setup(&d, p, 5, 0x30000u, 0x60000u, true);
    m_w32(NULL, 0x60000u, 0xe3a02007u);
    d.cp15.dfsr = 0xdeadbeefu; d.cp15.dfar = 0xcafebabeu;
    arm_step(&d);
    d.r[2] = 0x5a5a5a5au;
    arm_step(&d);
    CHECK(d.r[15] == 0x80001000u, "pc=%08x expect 80001000 before the fetch", d.r[15]);
    arm_step(&d);
    CHECK(d.r[15] == ARM_VEC_PREFETCH, "pc=%08x expect 0c (prefetch abort)", d.r[15]);
    CHECK((d.cp15.ifsr & 0xfu) == ARM_FSR_PAGE_PERMISSION,
          "ifsr=%08x expect a page permission fault — XN is reported as a "
          "permission fault", d.cp15.ifsr);
    CHECK((d.cp15.ifsr & (1u << 11)) == 0,
          "ifsr=%08x: IFSR has no WnR field", d.cp15.ifsr);
    CHECK(d.cp15.ifar == 0x80001000u, "ifar=%08x expect 80001000", d.cp15.ifar);
    CHECK(d.cp15.dfsr == 0xdeadbeefu && d.cp15.dfar == 0xcafebabeu,
          "an XN fault is a prefetch abort and must not touch the data side");
    CHECK(d.r[2] == 0x5a5a5a5au,
          "r2=%08x: the execute-never page must not have executed", d.r[2]);

    /* XN is a fetch-only attribute: the same page stays readable and writable.
     * If it did not, marking the heap non-executable would make it unusable. */
    uint32_t pa = 0;
    CHECK(arm_mmu_translate(&d, 0x80001000u, ARM_ACCESS_READ, true, &pa) == 0
          && pa == 0x60000u,
          "pa=%08x: an XN page must still translate for a read", pa);
    CHECK(arm_mmu_translate(&d, 0x80001000u, ARM_ACCESS_WRITE, true, &pa) == 0,
          "an XN page must still translate for a write");
    CHECK((arm_mmu_translate(&d, 0x80001000u, ARM_ACCESS_FETCH, true, &pa) & 0xfu)
          == ARM_FSR_PAGE_PERMISSION,
          "only ARM_ACCESS_FETCH may see the XN bit");

    /* A manager domain cannot generate a permission fault at all, and an XN
     * violation is a permission fault, so it is not checked there either. */
    d.cp15.dacr = 3u;
    CHECK(arm_mmu_translate(&d, 0x80001000u, ARM_ACCESS_FETCH, true, &pa) == 0,
          "a manager domain must not raise an XN fault");
}

static void test_xn_on_a_section_and_the_xp_gate(void) {
    /* Sections carry XN in bit 4. The section planted at index 0x800 maps
     * 0x80000000 onto physical 0, so 0x80000008 is the MOV r2,#7 below. */
    uint32_t p[] = { 0xe59f0008 /*LDR r0,[pc,#8]*/, 0xe1a0f000 /*MOV pc,r0*/,
                     0xe3a02007 /*MOV r2,#7     */, 0xeafffffe /*B .      */,
                     0x80000008u };
    struct { bool xn, xp; } cases[] = { {false, true}, {true, true}, {true, false} };
    for (unsigned k = 0; k < 3; k++) {
        arm_cpu_t c;
        memset(g_ram, 0, sizeof g_ram);
        for (unsigned i = 0; i < 5; i++) m_w32(NULL, i * 4, p[i]);
        m_w32(NULL, 0x4000 + (0x000u << 2), (3u << 10) | 2u);
        m_w32(NULL, 0x4000 + (0x800u << 2),
              (3u << 10) | 2u | (cases[k].xn ? (1u << 4) : 0u));
        arm_reset(&c, &g_bus);
        c.cp15.ttbr0  = 0x4000;
        c.cp15.dacr   = 1u;
        c.cp15.sctlr |= ARM_SCTLR_M | (cases[k].xp ? ARM_SCTLR_XP : 0u);
        c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
        c.r[15] = 0;
        c.r[2] = 0x5a5a5a5au;
        arm_step(&c); arm_step(&c); arm_step(&c);

        if (cases[k].xn && cases[k].xp) {
            CHECK(c.r[15] == ARM_VEC_PREFETCH,
                  "pc=%08x expect 0c: an XN section must abort on fetch", c.r[15]);
            CHECK((c.cp15.ifsr & 0xfu) == ARM_FSR_SECTION_PERMISSION,
                  "ifsr=%08x expect a SECTION permission fault", c.cp15.ifsr);
            CHECK(c.cp15.ifar == 0x80000008u, "ifar=%08x expect 80000008",
                  c.cp15.ifar);
        } else {
            /* xn && !xp is the inert case: with SCTLR.XP clear the descriptors
             * are the ARMv5-compatible layout, where bit 4 is not XN at all, so
             * reading it as XN would invent a fault the hardware never takes. */
            CHECK(c.r[2] == 7,
                  "r2=%08x expect 7 (xn=%d xp=%d): the fetch must be permitted",
                  c.r[2], (int)cases[k].xn, (int)cases[k].xp);
        }
    }
}

int main(void) {
    printf("iOS3-VM ARMv6 interpreter tests\n");
    test_mov_imm();
    test_add_reg();
    test_sub_flags();
    test_subs_negative();
    test_adds_overflow();
    test_barrel_lsl();
    test_branch();
    test_bl_sets_lr();
    test_ldr_str();
    test_ldrb();
    test_cond_not_taken();
    test_mul();
    test_orr_bic_mvn();
    test_bx_branches();
    test_exception_return_to_thumb_keeps_halfword();
    test_exception_return_to_arm_stays_word_aligned();
    test_ldm_exception_return_takes_state_from_spsr();
    test_rfe_aligns_for_the_restored_state();
    test_setend_le_runs_be_traps();
    test_umull_and_smull();
    test_umlal_accumulates();
    test_clz();
    test_qadd_saturates_and_sets_q();
    test_swp_exchanges();
    test_ldrexd_strexd_roundtrip();
    test_clrex_makes_strex_fail();
    test_exception_clears_exclusive_monitor();
    test_imm_dp_not_trapped();
    test_strh_ldrh();
    test_ldrsb_sign_extends();
    test_ldrsh_sign_extends();
    test_stmia_ldmia();
    test_push_pop();
    test_ldm_to_pc_branches();
    test_banked_sp_per_mode();
    test_fiq_banks_r8_r12();
    test_mrs_reads_cpsr();
    test_msr_switches_mode();
    test_swi_enters_svc();
    test_exception_return_restores_mode();
    test_cp15_reads_midr();
    test_cp15_cpuid_feature_bank();
    test_cp15_cpuid_scheme_grades_as_armv6();
    test_cp15_id_dfr0_matches_absent_debug_unit();
    test_cp15_sctlr_roundtrip();
    test_cp15_ttbr0_roundtrip();
    test_cp15_cache_op_is_accepted();
    test_high_vectors();
    test_mmu_disabled_is_identity();
    test_mmu_section_translation();
    test_mmu_unmapped_faults();
    test_mmu_user_write_permission();
    test_mmu_small_page_translation();
    test_data_abort_taken();
    test_pld_is_a_nop_not_a_branch();
    test_unconditional_space_traps();
    test_clz_does_not_corrupt_cpsr();
    test_msr_still_works();
    test_arm_media_extend_and_reverse();
    test_unimplemented_media_still_traps();
    test_apx_makes_mapping_read_only();
    test_abort_restores_base_register();
    test_thumb_mov_add();
    test_thumb_lsl_flags();
    test_thumb_push_pop();
    test_thumb_conditional_branch();
    test_arm_to_thumb_and_back();
    test_thumb_bl_pair();
    test_thumb_extend_and_reverse();
    test_thumb_rev();
    test_thumb_cps();
    test_mmu_supersection();
    test_thumb_blx_suffix_is_not_a_branch();
    test_user_bank_stm();
    test_mmu_ttbcr_n0_ignores_ttbr1();
    test_mmu_ttbcr_n2_low_va_uses_shrunk_ttbr0();
    test_mmu_ttbcr_n2_kernel_va_uses_ttbr1();
    test_mmu_ttbcr_n2_split_boundary();
    test_mmu_kernel_mapping_survives_ttbr0_pmap_switch();
    test_mmu_ttbcr_n1_table_geometry();
    test_mmu_ttbcr_n3_table_geometry();
    test_dfsr_wnr_write_vs_read();
    test_data_abort_write_sets_dfsr_wnr();
    test_prefetch_abort_never_sets_wnr();
    test_dfar_is_the_faulting_word_of_a_block_transfer();
    test_cps_is_a_nop_in_user_mode();
    test_unaligned_access_spanning_two_pages();
    test_unaligned_access_faulting_on_the_second_page();
    test_xn_blocks_fetch_from_a_small_page();
    test_xn_on_a_section_and_the_xp_gate();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

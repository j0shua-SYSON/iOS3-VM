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

static void test_swp_traps(void) {
    /* SWP r0,r1,[r2] (0xe1020091) is in the extension space with bits[6:5]==00
     * and is not implemented; it must trap rather than run as data-processing. */
    uint32_t p[] = { 0xe1020091 };
    arm_cpu_t c; arm_status_t st = run_status(&c, p, 1, 1);
    CHECK(st == ARM_UNDEFINED, "status=%d expect ARM_UNDEFINED for SWP", (int)st);
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

static void test_cp15_sctlr_roundtrip(void) {
    /* MOV r0,#1 ; MCR c1 (SCTLR <- 1, MMU enable bit) ; MRC c1 -> r1 */
    uint32_t p[] = { 0xe3a00001 /*MOV r0,#1*/,
                     0xee010f10 /*MCR p15,0,r0,c1,c0,0*/,
                     0xee111f10 /*MRC p15,0,r1,c1,c0,0*/ };
    arm_cpu_t c; load_and_run(&c, p, 3, 3);
    CHECK(c.r[1] == 1, "r1=%08x expect 1 (SCTLR readback)", c.r[1]);
    CHECK((c.cp15.sctlr & ARM_SCTLR_M) != 0, "SCTLR.M should be set");
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
    test_swp_traps();
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
    test_cp15_sctlr_roundtrip();
    test_cp15_ttbr0_roundtrip();
    test_cp15_cache_op_is_accepted();
    test_high_vectors();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

/*
 * iOS3-VM — byte-exact tests for the AArch64 emitter.
 *
 * The dev box is x86, so emitted arm64 code cannot be *executed* here — but it
 * can be checked exactly, which is most of the value. Every case below asserts
 * the emitted 32-bit word against an encoding derived from the ARMv8-A field
 * layout and cross-checked against the disassembly named in the comment.
 *
 * The bitmask-immediate encoder gets a stronger test than hand-picked cases:
 * DECODE_BITMASKS from the ARM ARM is implemented independently at the bottom
 * of this file and the two are checked against each other exhaustively over the
 * whole (N, immr, imms) space, plus a randomised sweep in the other direction
 * to prove the encoder never accepts a value the architecture cannot express.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "a64_emit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* Emit exactly one instruction with `call` and compare it to `want`. */
static uint32_t g_buf[64];
static a64_emit_t g_e;
static void reset(void) { a64_init(&g_e, g_buf, sizeof g_buf / sizeof g_buf[0]); }

#define ONE(want, text, call) do {                                            \
    reset(); call;                                                            \
    if (g_e.n != 1u) { g_fail++; printf("  FAIL %s:%d: %s emitted %u words\n", \
                       __func__, __LINE__, text, (unsigned)g_e.n); }          \
    else if (g_buf[0] != (uint32_t)(want)) { g_fail++;                        \
        printf("  FAIL %s:%d: %s = %08x, expected %08x\n", __func__, __LINE__, \
               text, g_buf[0], (uint32_t)(want)); }                           \
    else if (!a64_ok(&g_e)) { g_fail++;                                       \
        printf("  FAIL %s:%d: %s flagged bad\n", __func__, __LINE__, text); } \
    else g_pass++;                                                            \
} while (0)

/* Assert that a request is *refused* rather than mis-encoded. */
#define REFUSED(text, call) do {                                              \
    reset(); call;                                                            \
    if (a64_ok(&g_e)) { g_fail++;                                             \
        printf("  FAIL %s:%d: %s should have been refused\n",                 \
               __func__, __LINE__, text); }                                   \
    else g_pass++;                                                            \
} while (0)

/* ------------------------------------------------------------- move wide */

static void test_move_wide(void) {
    ONE(0x52800540, "movz w0, #42",              a64_movz(&g_e, A64_W, 0, 42, 0));
    ONE(0xd2800000, "movz x0, #0",               a64_movz(&g_e, A64_X, 0, 0, 0));
    ONE(0x529fffe1, "movz w1, #0xffff",          a64_movz(&g_e, A64_W, 1, 0xffff, 0));
    ONE(0x529fffe0, "movz w0, #0xffff",          a64_movz(&g_e, A64_W, 0, 0xffff, 0));
    ONE(0x52bfffe0, "movz w0, #0xffff, lsl #16", a64_movz(&g_e, A64_W, 0, 0xffff, 16));
    ONE(0xd2c24682, "movz x2, #0x1234, lsl #32", a64_movz(&g_e, A64_X, 2, 0x1234, 32));
    ONE(0x12800000, "movn w0, #0",               a64_movn(&g_e, A64_W, 0, 0, 0));
    ONE(0x92800000, "movn x0, #0",               a64_movn(&g_e, A64_X, 0, 0, 0));
    ONE(0x72a24680, "movk w0, #0x1234, lsl #16", a64_movk(&g_e, A64_W, 0, 0x1234, 16));

    /* A 32-bit move-wide has no lsl #32 form, and imm16 is 16 bits. */
    REFUSED("movz w0, #0, lsl #32", a64_movz(&g_e, A64_W, 0, 0, 32));
    REFUSED("movz x0, #0x10000",    a64_movz(&g_e, A64_X, 0, 0x10000, 0));
    REFUSED("movz x0, #0, lsl #8",  a64_movz(&g_e, A64_X, 0, 0, 8));
}

static void expect_seq(const char *text, const uint32_t *want, unsigned n) {
    unsigned i;
    if (g_e.n != n) {
        g_fail++;
        printf("  FAIL %s: emitted %u words, expected %u\n", text, (unsigned)g_e.n, n);
        return;
    }
    for (i = 0; i < n; i++) {
        if (g_buf[i] != want[i]) {
            g_fail++;
            printf("  FAIL %s: word %u = %08x, expected %08x\n", text, i, g_buf[i], want[i]);
            return;
        }
    }
    g_pass++;
}

static void test_mov_imm_sequences(void) {
    { uint32_t w[] = { 0x52800540 };
      reset(); a64_mov_imm(&g_e, A64_W, 0, 42);          expect_seq("mov w0,#42", w, 1); }
    { uint32_t w[] = { 0xd2800000 };
      reset(); a64_mov_imm(&g_e, A64_X, 0, 0);           expect_seq("mov x0,#0", w, 1); }
    { uint32_t w[] = { 0x12800000 };
      reset(); a64_mov_imm(&g_e, A64_W, 0, 0xffffffffu); expect_seq("mov w0,#-1", w, 1); }
    { uint32_t w[] = { 0x92800000 };
      reset(); a64_mov_imm(&g_e, A64_X, 0, ~(uint64_t)0);expect_seq("mov x0,#-1", w, 1); }
    /* movz w0,#0x5678 ; movk w0,#0x1234,lsl#16 */
    { uint32_t w[] = { 0x528acf00, 0x72a24680 };
      reset(); a64_mov_imm(&g_e, A64_W, 0, 0x12345678u); expect_seq("mov w0,#0x12345678", w, 2); }
    /* Only the top halfword is interesting: one movz with a shift. */
    { uint32_t w[] = { 0x529fffe0 };
      reset(); a64_mov_imm(&g_e, A64_W, 0, 0x0000ffffu); expect_seq("mov w0,#0xffff", w, 1); }
    { uint32_t w[] = { 0x52bfffe0 };
      reset(); a64_mov_imm(&g_e, A64_W, 0, 0xffff0000u); expect_seq("mov w0,#0xffff0000", w, 1); }
    /* Mostly-ones picks MOVN: ~0x0000000f == 0xfffffff0. */
    { uint32_t w[] = { 0x128001e0 };
      reset(); a64_mov_imm(&g_e, A64_W, 0, 0xfffffff0u); expect_seq("mov w0,#0xfffffff0", w, 1); }
    { uint32_t w[] = { 0x92bfffe0 };
      reset(); a64_mov_imm(&g_e, A64_X, 0, 0xffffffff0000ffffull);
      expect_seq("mov x0,#0xffffffff0000ffff", w, 1); }

    CHECK(a64_mov_imm_words(A64_W, 42) == 1, "words(42)");
    CHECK(a64_mov_imm_words(A64_W, 0x12345678u) == 2, "words(0x12345678)");
    CHECK(a64_mov_imm_words(A64_X, 0x0123456789abcdefull) == 4, "words(64-bit)");
}

/* --------------------------------------------------- add / subtract ----- */

static void test_addsub_imm(void) {
    ONE(0x91000400, "add x0, x0, #1",        a64_add_imm(&g_e, A64_X, 0, 0, 1, false));
    ONE(0x91400400, "add x0, x0, #1, lsl#12",a64_add_imm(&g_e, A64_X, 0, 0, 1, true));
    ONE(0xd10043ff, "sub sp, sp, #16",       a64_sub_imm(&g_e, A64_X, A64_SP, A64_SP, 16, false));
    ONE(0x313ffc41, "adds w1, w2, #0xfff",   a64_adds_imm(&g_e, A64_W, 1, 2, 0xfff, false));
    ONE(0x7100041f, "cmp w0, #1",            a64_cmp_imm(&g_e, A64_W, 0, 1, false));
    ONE(0x3100041f, "cmn w0, #1",            a64_cmn_imm(&g_e, A64_W, 0, 1, false));
    ONE(0x910003e0, "mov x0, sp",            a64_mov_sp(&g_e, A64_X, 0, A64_SP));
    REFUSED("add x0,x0,#4096", a64_add_imm(&g_e, A64_X, 0, 0, 4096, false));

    CHECK(a64_addsub_imm_fits(4095), "4095 fits");
    CHECK(a64_addsub_imm_fits(0x1000), "0x1000 fits (shifted)");
    CHECK(!a64_addsub_imm_fits(0x1001), "0x1001 does not fit");
}

static void test_addsub_reg(void) {
    ONE(0x8b020020, "add x0, x1, x2",          a64_add_reg(&g_e, A64_X, 0, 1, 2, A64_LSL, 0));
    ONE(0x4b020020, "sub w0, w1, w2",          a64_sub_reg(&g_e, A64_W, 0, 1, 2, A64_LSL, 0));
    ONE(0xeb01001f, "cmp x0, x1",              a64_cmp_reg(&g_e, A64_X, 0, 1, A64_LSL, 0));
    ONE(0x2b021020, "adds w0, w1, w2, lsl #4", a64_adds_reg(&g_e, A64_W, 0, 1, 2, A64_LSL, 4));
    ONE(0xeb851c83, "subs x3, x4, x5, asr #7", a64_subs_reg(&g_e, A64_X, 3, 4, 5, A64_ASR, 7));
    ONE(0x4b0103e0, "neg w0, w1",              a64_neg_reg(&g_e, A64_W, 0, 1));
    ONE(0x2b01001f, "cmn w0, w1",              a64_cmn_reg(&g_e, A64_W, 0, 1, A64_LSL, 0));
    /* ROR is not an add/sub shift, and a 32-bit shift amount is 5 bits. */
    REFUSED("add w0,w1,w2,ror#1", a64_add_reg(&g_e, A64_W, 0, 1, 2, A64_ROR, 1));
    REFUSED("add w0,w1,w2,lsl#32", a64_add_reg(&g_e, A64_W, 0, 1, 2, A64_LSL, 32));
}

static void test_adc_sbc(void) {
    ONE(0x1a020020, "adc w0, w1, w2",  a64_adc (&g_e, A64_W, 0, 1, 2));
    ONE(0x3a020020, "adcs w0, w1, w2", a64_adcs(&g_e, A64_W, 0, 1, 2));
    ONE(0x5a020020, "sbc w0, w1, w2",  a64_sbc (&g_e, A64_W, 0, 1, 2));
    ONE(0x7a020020, "sbcs w0, w1, w2", a64_sbcs(&g_e, A64_W, 0, 1, 2));
    ONE(0x9a020020, "adc x0, x1, x2",  a64_adc (&g_e, A64_X, 0, 1, 2));
}

/* -------------------------------------------------------- logical ------- */

static void test_logical_reg(void) {
    ONE(0xaa0103e0, "mov x0, x1",              a64_mov_reg(&g_e, A64_X, 0, 1));
    ONE(0x2a0103e0, "mov w0, w1",              a64_mov_reg(&g_e, A64_W, 0, 1));
    ONE(0x0a020020, "and w0, w1, w2",          a64_and_reg (&g_e, A64_W, 0, 1, 2, A64_LSL, 0));
    ONE(0x2a020020, "orr w0, w1, w2",          a64_orr_reg (&g_e, A64_W, 0, 1, 2, A64_LSL, 0));
    ONE(0x4a020020, "eor w0, w1, w2",          a64_eor_reg (&g_e, A64_W, 0, 1, 2, A64_LSL, 0));
    ONE(0x6a020020, "ands w0, w1, w2",         a64_ands_reg(&g_e, A64_W, 0, 1, 2, A64_LSL, 0));
    ONE(0x8a220020, "bic x0, x1, x2",          a64_bic_reg (&g_e, A64_X, 0, 1, 2, A64_LSL, 0));
    ONE(0x6a22001f, "bics wzr, w0, w2",        a64_bics_reg(&g_e, A64_W, A64_ZR, 0, 2, A64_LSL, 0));
    ONE(0x2a2103e0, "mvn w0, w1",              a64_mvn_reg (&g_e, A64_W, 0, 1));
    ONE(0x6a01001f, "tst w0, w1",              a64_tst_reg (&g_e, A64_W, 0, 1, A64_LSL, 0));
    ONE(0x2ac22020, "orr w0, w1, w2, ror #8",  a64_orr_reg (&g_e, A64_W, 0, 1, 2, A64_ROR, 8));
    ONE(0x4a621020, "eon w0, w1, w2, lsr #4",  a64_eon_reg (&g_e, A64_W, 0, 1, 2, A64_LSR, 4));
    /* ANDS Wd,Wn,Wn is how the translator recomputes N and Z (see §5.2). */
    ONE(0x6a13027f, "ands wzr, w19, w19",      a64_ands_reg(&g_e, A64_W, A64_ZR, 19, 19, A64_LSL, 0));
}

static void test_logical_imm(void) {
    ONE(0x12001c00, "and w0, w0, #0xff",       a64_and_imm (&g_e, A64_W, 0, 0, 0xff));
    ONE(0x92402c00, "and x0, x0, #0xfff",      a64_and_imm (&g_e, A64_X, 0, 0, 0xfff));
    ONE(0x320003e0, "orr w0, wzr, #1",         a64_orr_imm (&g_e, A64_W, 0, A64_ZR, 1));
    ONE(0x72010041, "ands w1, w2, #0x80000000",a64_ands_imm(&g_e, A64_W, 1, 2, 0x80000000u));
    ONE(0xf240001f, "tst x0, #1",              a64_tst_imm (&g_e, A64_X, 0, 1));
    ONE(0x52000400, "eor w0, w0, #3",          a64_eor_imm (&g_e, A64_W, 0, 0, 3));
    /* Values that are not a rotated run of ones must be refused, not folded. */
    REFUSED("and w0,w0,#0",          a64_and_imm(&g_e, A64_W, 0, 0, 0));
    REFUSED("and w0,w0,#0xffffffff", a64_and_imm(&g_e, A64_W, 0, 0, 0xffffffffu));
    REFUSED("and w0,w0,#0x12345678", a64_and_imm(&g_e, A64_W, 0, 0, 0x12345678u));
    REFUSED("and w0,w0,#0x1ffffffff",a64_and_imm(&g_e, A64_W, 0, 0, 0x1ffffffffull));
}

/* ------------------------------------------- shifts, bitfield, select --- */

static void test_shifts(void) {
    ONE(0x1ac22020, "lsl w0, w1, w2", a64_lslv(&g_e, A64_W, 0, 1, 2));
    ONE(0x9ac22420, "lsr x0, x1, x2", a64_lsrv(&g_e, A64_X, 0, 1, 2));
    ONE(0x1ac22820, "asr w0, w1, w2", a64_asrv(&g_e, A64_W, 0, 1, 2));
    ONE(0x1ac22c20, "ror w0, w1, w2", a64_rorv(&g_e, A64_W, 0, 1, 2));
    ONE(0x5ac01020, "clz w0, w1",     a64_clz (&g_e, A64_W, 0, 1));
    ONE(0xdac01020, "clz x0, x1",     a64_clz (&g_e, A64_X, 0, 1));
}

static void test_bitfield(void) {
    ONE(0x53047c20, "lsr w0, w1, #4",     a64_lsr_imm(&g_e, A64_W, 0, 1, 4));
    ONE(0x531c6c20, "lsl w0, w1, #4",     a64_lsl_imm(&g_e, A64_W, 0, 1, 4));
    ONE(0x13047c20, "asr w0, w1, #4",     a64_asr_imm(&g_e, A64_W, 0, 1, 4));
    ONE(0xd344fc20, "lsr x0, x1, #4",     a64_lsr_imm(&g_e, A64_X, 0, 1, 4));
    ONE(0x531d7420, "ubfx w0, w1, #29,#1",a64_ubfx(&g_e, A64_W, 0, 1, 29, 1));
    ONE(0x131d7420, "sbfx w0, w1, #29,#1",a64_sbfx(&g_e, A64_W, 0, 1, 29, 1));
    ONE(0x33030020, "bfi w0, w1, #29, #1",a64_bfi (&g_e, A64_W, 0, 1, 29, 1));
    /* The two the flag-fixup sequence of §5.2 needs. */
    ONE(0xd35c7529, "ubfx x9, x9, #28,#2",a64_ubfx(&g_e, A64_X, 9, 9, 28, 2));
    ONE(0xb364052a, "bfi x10, x9, #28,#2",a64_bfi (&g_e, A64_X, 10, 9, 28, 2));
    REFUSED("lsl w0,w1,#32",   a64_lsl_imm(&g_e, A64_W, 0, 1, 32));
    REFUSED("ubfx w0,w1,#30,#4",a64_ubfx(&g_e, A64_W, 0, 1, 30, 4));
    REFUSED("ubfx w0,w1,#0,#0", a64_ubfx(&g_e, A64_W, 0, 1, 0, 0));
}

static void test_condsel(void) {
    ONE(0x1a9f17e0, "cset w0, eq",          a64_cset (&g_e, A64_W, 0, A64_EQ));
    ONE(0x1a821020, "csel w0, w1, w2, ne",  a64_csel (&g_e, A64_W, 0, 1, 2, A64_NE));
    ONE(0x9a82a420, "csinc x0, x1, x2, ge", a64_csinc(&g_e, A64_X, 0, 1, 2, A64_GE));
    REFUSED("cset w0, al", a64_cset(&g_e, A64_W, 0, A64_AL));
}

static void test_multiply(void) {
    ONE(0x1b027c20, "mul w0, w1, w2",       a64_mul (&g_e, A64_W, 0, 1, 2));
    ONE(0x9b020c20, "madd x0, x1, x2, x3",  a64_madd(&g_e, A64_X, 0, 1, 2, 3));
}

/* ------------------------------------------------------ load / store ---- */

static void test_ldst_uimm(void) {
    ONE(0xb9400020, "ldr w0, [x1]",       a64_ldr_uimm(&g_e, A64_SZ_W, 0, 1, 0));
    ONE(0xb9000420, "str w0, [x1, #4]",   a64_str_uimm(&g_e, A64_SZ_W, 0, 1, 4));
    ONE(0xf9400420, "ldr x0, [x1, #8]",   a64_ldr_uimm(&g_e, A64_SZ_D, 0, 1, 8));
    ONE(0x39400420, "ldrb w0, [x1, #1]",  a64_ldr_uimm(&g_e, A64_SZ_B, 0, 1, 1));
    ONE(0x39000020, "strb w0, [x1]",      a64_str_uimm(&g_e, A64_SZ_B, 0, 1, 0));
    ONE(0x79400420, "ldrh w0, [x1, #2]",  a64_ldr_uimm(&g_e, A64_SZ_H, 0, 1, 2));
    ONE(0x79000420, "strh w0, [x1, #2]",  a64_str_uimm(&g_e, A64_SZ_H, 0, 1, 2));
    ONE(0x39c00020, "ldrsb w0, [x1]",     a64_ldrs_uimm(&g_e, A64_SZ_B, A64_W, 0, 1, 0));
    ONE(0x39800020, "ldrsb x0, [x1]",     a64_ldrs_uimm(&g_e, A64_SZ_B, A64_X, 0, 1, 0));
    ONE(0x79800020, "ldrsh x0, [x1]",     a64_ldrs_uimm(&g_e, A64_SZ_H, A64_X, 0, 1, 0));
    ONE(0xb9800020, "ldrsw x0, [x1]",     a64_ldrs_uimm(&g_e, A64_SZ_W, A64_X, 0, 1, 0));
    ONE(0xb9000393, "str w19, [x28]",     a64_str_uimm(&g_e, A64_SZ_W, 19, 28, 0));
    ONE(0xb97ffc20, "ldr w0, [x1, #16380]",a64_ldr_uimm(&g_e, A64_SZ_W, 0, 1, 16380));
    REFUSED("ldr w0,[x1,#2]",     a64_ldr_uimm(&g_e, A64_SZ_W, 0, 1, 2));
    REFUSED("ldr w0,[x1,#16384]", a64_ldr_uimm(&g_e, A64_SZ_W, 0, 1, 16384));
    REFUSED("ldrsw w0,[x1]",      a64_ldrs_uimm(&g_e, A64_SZ_W, A64_W, 0, 1, 0));
    REFUSED("ldrsd x0,[x1]",      a64_ldrs_uimm(&g_e, A64_SZ_D, A64_X, 0, 1, 0));
}

static void test_ldst_indexed(void) {
    ONE(0xf81f0ffe, "str x30, [sp, #-16]!", a64_str_pre (&g_e, A64_SZ_D, 30, A64_SP, -16));
    ONE(0xf84107e0, "ldr x0, [sp], #16",    a64_ldr_post(&g_e, A64_SZ_D, 0, A64_SP, 16));
    ONE(0xf85f8020, "ldur x0, [x1, #-8]",   a64_ldur    (&g_e, A64_SZ_D, 0, 1, -8));
    ONE(0xb8004020, "stur w0, [x1, #4]",    a64_stur    (&g_e, A64_SZ_W, 0, 1, 4));
    REFUSED("ldur x0,[x1,#256]",  a64_ldur(&g_e, A64_SZ_D, 0, 1, 256));
    REFUSED("ldur x0,[x1,#-257]", a64_ldur(&g_e, A64_SZ_D, 0, 1, -257));
}

static void test_ldst_reg(void) {
    ONE(0xb8626820, "ldr w0, [x1, x2]",          a64_ldr_reg(&g_e, A64_SZ_W, 0, 1, 2, A64_EXT_LSL, 0));
    ONE(0xf8627820, "ldr x0, [x1, x2, lsl #3]",  a64_ldr_reg(&g_e, A64_SZ_D, 0, 1, 2, A64_EXT_LSL, 3));
    ONE(0xb8226820, "str w0, [x1, x2]",          a64_str_reg(&g_e, A64_SZ_W, 0, 1, 2, A64_EXT_LSL, 0));
    ONE(0x38624820, "ldrb w0, [x1, w2, uxtw]",   a64_ldr_reg(&g_e, A64_SZ_B, 0, 1, 2, A64_EXT_UXTW, 0));
    REFUSED("ldr w0,[x1,x2,lsl#1]", a64_ldr_reg(&g_e, A64_SZ_W, 0, 1, 2, A64_EXT_LSL, 1));
}

static void test_ldst_pair(void) {
    ONE(0xa9bf7bfd, "stp x29, x30, [sp, #-16]!", a64_stp_pre (&g_e, A64_SZ_D, 29, 30, A64_SP, -16));
    ONE(0xa8c17bfd, "ldp x29, x30, [sp], #16",   a64_ldp_post(&g_e, A64_SZ_D, 29, 30, A64_SP, 16));
    ONE(0xa90153f3, "stp x19, x20, [sp, #16]",   a64_stp     (&g_e, A64_SZ_D, 19, 20, A64_SP, 16));
    ONE(0x29415393, "ldp w19, w20, [x28, #8]",   a64_ldp     (&g_e, A64_SZ_W, 19, 20, 28, 8));
    ONE(0x29005393, "stp w19, w20, [x28]",       a64_stp     (&g_e, A64_SZ_W, 19, 20, 28, 0));
    REFUSED("stp x0,x1,[sp,#4]",   a64_stp(&g_e, A64_SZ_D, 0, 1, A64_SP, 4));
    REFUSED("stp x0,x1,[sp,#512]", a64_stp(&g_e, A64_SZ_D, 0, 1, A64_SP, 512));
    REFUSED("stp b0,b1,[sp]",      a64_stp(&g_e, A64_SZ_B, 0, 1, A64_SP, 0));
}

/* ---------------------------------------------------------- branches ---- */

static void test_branches(void) {
    ONE(0x14000004, "b .+16",       a64_b(&g_e, 4));
    ONE(0x17ffffff, "b .-4",        a64_b(&g_e, -1));
    ONE(0x94000002, "bl .+8",       a64_bl(&g_e, 2));
    ONE(0x54000060, "b.eq .+12",    a64_bcond(&g_e, A64_EQ, 3));
    ONE(0x54ffffc1, "b.ne .-8",     a64_bcond(&g_e, A64_NE, -2));
    ONE(0x34000040, "cbz w0, .+8",  a64_cbz(&g_e, A64_W, 0, 2));
    ONE(0xb5000041, "cbnz x1, .+8", a64_cbnz(&g_e, A64_X, 1, 2));
    ONE(0x36180040, "tbz w0, #3, .+8",  a64_tbz(&g_e, 0, 3, 2));
    ONE(0xb7400020, "tbnz x0, #40, .+4",a64_tbnz(&g_e, 0, 40, 1));
    ONE(0xd61f0100, "br x8",        a64_br(&g_e, 8));
    ONE(0xd63f0100, "blr x8",       a64_blr(&g_e, 8));
    ONE(0xd65f03c0, "ret",          a64_ret(&g_e, 30));
    REFUSED("b .+2^28", a64_b(&g_e, 1 << 26));
    REFUSED("b.eq far", a64_bcond(&g_e, A64_EQ, 1 << 19));
}

static void test_bind_and_patch(void) {
    size_t site;
    reset();
    a64_bcond(&g_e, A64_NE, 0);      /* placeholder, patched below */
    site = 0;
    a64_nop(&g_e);
    a64_nop(&g_e);
    a64_bind(&g_e, site);            /* target is word 3 => displacement +3 */
    CHECK(a64_ok(&g_e), "bind succeeded");
    CHECK(g_buf[0] == 0x54000061u, "b.ne bound to +3 = %08x", g_buf[0]);

    reset();
    a64_b(&g_e, 0);
    a64_nop(&g_e);
    a64_bind(&g_e, 0);
    CHECK(g_buf[0] == 0x14000002u, "b bound to +2 = %08x", g_buf[0]);

    reset();
    a64_cbz(&g_e, A64_W, 5, 0);
    a64_nop(&g_e);
    a64_nop(&g_e);
    a64_nop(&g_e);
    a64_bind(&g_e, 0);
    CHECK(g_buf[0] == 0x34000085u, "cbz bound to +4 = %08x", g_buf[0]);

    /* Patching something that is not a branch must fail loudly. */
    { uint32_t w = 0xd503201fu;
      CHECK(!a64_patch_branch(&w, 1), "nop is not patchable as a branch"); }
    { uint32_t w = 0x14000000u;
      CHECK(a64_patch_branch(&w, -3) && w == 0x17fffffdu, "b patched to -3 = %08x", w); }
}

/* ------------------------------------------------------------- system --- */

static void test_system(void) {
    ONE(0xd53b4200, "mrs x0, nzcv",  a64_mrs_nzcv(&g_e, 0));
    ONE(0xd51b4209, "msr nzcv, x9",  a64_msr_nzcv(&g_e, 9));
    ONE(0xd53b4200, "mrs x0, s3_3_c4_c2_0", a64_mrs(&g_e, 0, 3, 3, 4, 2, 0));
    ONE(0xd503201f, "nop",           a64_nop(&g_e));
    ONE(0xd4200000, "brk #0",        a64_brk(&g_e, 0));
    ONE(0xd4200020, "brk #1",        a64_brk(&g_e, 1));
}

/* --------------------------------------------------- buffer discipline -- */

static void test_overflow_is_latched(void) {
    uint32_t small[2];
    a64_emit_t e;
    a64_init(&e, small, 2);
    a64_nop(&e); a64_nop(&e);
    CHECK(a64_ok(&e), "two words fit");
    a64_nop(&e);
    CHECK(!a64_ok(&e) && e.overflow, "third word overflows and is latched");
    CHECK(e.n == 2, "no write past the end (n=%u)", (unsigned)e.n);
    CHECK(a64_size_bytes(&e) == 8, "size in bytes");
}

static void test_bad_register_is_refused(void) {
    REFUSED("add w0,w1,w32", a64_add_reg(&g_e, A64_W, 0, 1, 32, A64_LSL, 0));
    REFUSED("ldr w99,[x1]",  a64_ldr_uimm(&g_e, A64_SZ_W, 99, 1, 0));
}

/* ============================================================= bitmask ===
 * An independent implementation of DecodeBitMasks() from the ARM ARM, used to
 * check a64_bitmask_imm() from the other direction. `immediate` is TRUE (the
 * logical-immediate case), so the all-ones element pattern is reserved.
 */
static bool decode_bitmask(int sf, unsigned n, unsigned immr, unsigned imms,
                           uint64_t *out) {
    unsigned regsize = sf ? 64u : 32u;
    unsigned combined = (n << 6) | ((~imms) & 0x3fu);
    int      len = -1;
    unsigned levels, S, R, esize, i;
    uint64_t welem, rot, val;

    for (i = 0; i < 7u; i++) if (combined & (1u << i)) len = (int)i;
    if (len < 1) return false;
    if ((unsigned)(1u << len) > regsize) return false;   /* esize must fit */

    levels = (1u << len) - 1u;
    if ((imms & levels) == levels) return false;         /* reserved */

    S = imms & levels;
    R = immr & levels;
    esize = 1u << len;

    welem = (S + 1u >= 64u) ? ~(uint64_t)0 : (((uint64_t)1 << (S + 1u)) - 1u);
    /* ROR the element right by R within esize bits. */
    { uint64_t emask = (esize >= 64u) ? ~(uint64_t)0 : (((uint64_t)1 << esize) - 1u);
      welem &= emask;
      rot = R ? (((welem >> R) | (welem << (esize - R))) & emask) : welem; }

    val = 0;
    for (i = 0; i < regsize; i += esize) val |= rot << i;
    if (regsize == 32u) val &= 0xffffffffu;
    *out = val;
    return true;
}

/* A sorted, de-duplicated list of every value the architecture can express. */
#define MAX_LEGAL 8192
static uint64_t g_legal[MAX_LEGAL];
static unsigned g_nlegal;

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}
static bool legal_contains(uint64_t v) {
    unsigned lo = 0, hi = g_nlegal;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2u;
        if (g_legal[mid] == v) return true;
        if (g_legal[mid] < v) lo = mid + 1u; else hi = mid;
    }
    return false;
}

/*
 * Direction 1: every (N, immr, imms) the architecture accepts produces a value
 * the encoder must accept, and re-encoding it must decode back to the same
 * value. (The encoder may legitimately pick a different-but-equivalent triple.)
 */
static void test_bitmask_all_encodings(int sf) {
    unsigned n, immr, imms, checked = 0, mismatches = 0;
    g_nlegal = 0;
    for (n = 0; n < 2u; n++)
      for (immr = 0; immr < 64u; immr++)
        for (imms = 0; imms < 64u; imms++) {
            uint64_t v, back; uint32_t enc;
            if (!decode_bitmask(sf, n, immr, imms, &v)) continue;
            checked++;
            if (g_nlegal < MAX_LEGAL) g_legal[g_nlegal++] = v;
            if (!a64_bitmask_imm(sf, v, &enc)) { mismatches++; continue; }
            if (!decode_bitmask(sf, (enc >> 12) & 1u, (enc >> 6) & 0x3fu,
                                enc & 0x3fu, &back) || back != v) mismatches++;
        }
    CHECK(checked > 1000, "%s: %u legal encodings enumerated", sf ? "64-bit" : "32-bit", checked);
    CHECK(mismatches == 0, "%s: %u encoder round-trip mismatches", sf ? "64-bit" : "32-bit", mismatches);

    qsort(g_legal, g_nlegal, sizeof g_legal[0], cmp_u64);
    { unsigned i, w = 0;
      for (i = 0; i < g_nlegal; i++) if (i == 0 || g_legal[i] != g_legal[i-1]) g_legal[w++] = g_legal[i];
      g_nlegal = w; }
}

/*
 * Direction 2: the encoder must never accept a value the architecture cannot
 * express. Sweep pseudo-random values (and all 16-bit values, which contain
 * most of the interesting boundaries) against the enumerated legal set.
 */
static void test_bitmask_rejects(int sf) {
    uint64_t state = 0x243f6a8885a308d3ull;
    unsigned i, wrong_accept = 0, wrong_reject = 0;
    for (i = 0; i < 400000u; i++) {
        uint64_t v; uint32_t enc; bool got, want;
        if (i < 65536u) v = i;
        else {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            v = state >> 1;
            if (!sf) v &= 0xffffffffu;
            /* Bias towards masks and near-masks, where the boundaries live. */
            if (i & 1u) v = (v & 0x3f) ? ((uint64_t)1 << (v & 0x3f)) - 1u : 0u;
            if (i % 7u == 0) v = ~v;
            if (!sf) v &= 0xffffffffu;
        }
        want = legal_contains(v);
        got  = a64_bitmask_imm(sf, v, &enc);
        if (got && !want) wrong_accept++;
        if (!got && want) wrong_reject++;
        if (got) {
            uint64_t back;
            if (!decode_bitmask(sf, (enc >> 12) & 1u, (enc >> 6) & 0x3fu, enc & 0x3fu, &back)
                || back != v) wrong_accept++;
        }
    }
    CHECK(wrong_accept == 0, "%s: %u values wrongly accepted", sf ? "64-bit" : "32-bit", wrong_accept);
    CHECK(wrong_reject == 0, "%s: %u values wrongly rejected", sf ? "64-bit" : "32-bit", wrong_reject);
}

static void test_bitmask(void) {
    test_bitmask_all_encodings(A64_W);
    test_bitmask_rejects(A64_W);
    test_bitmask_all_encodings(A64_X);
    test_bitmask_rejects(A64_X);
}

int main(void) {
    printf("a64 emitter\n");
    test_move_wide();
    test_mov_imm_sequences();
    test_addsub_imm();
    test_addsub_reg();
    test_adc_sbc();
    test_logical_reg();
    test_logical_imm();
    test_shifts();
    test_bitfield();
    test_condsel();
    test_multiply();
    test_ldst_uimm();
    test_ldst_indexed();
    test_ldst_reg();
    test_ldst_pair();
    test_branches();
    test_bind_and_patch();
    test_system();
    test_overflow_is_latched();
    test_bad_register_is_refused();
    test_bitmask();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

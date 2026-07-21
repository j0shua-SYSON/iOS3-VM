/*
 * iOS3-VM — block translator tests.
 *
 * The dev box cannot execute arm64, so these tests check the two things that
 * are checkable off-target and that matter most:
 *
 *   1. BLOCK SHAPE — that a block stops exactly where docs/dynarec.md §3.2
 *      says it must, and that everything outside the (small) native set is
 *      handed to the interpreter rather than mis-translated. This is the
 *      property that makes the JIT safe to enable incrementally.
 *   2. EMITTED CODE — byte-exact assertions on the fixed prologue and on the
 *      body emitted for representative guest instructions.
 *
 * What these tests cannot show is that the emitted code *computes* the right
 * answer. That needs an arm64 host; see core/tests/test_jit_exec.c and the
 * arm64 CI job.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "jit.h"
#include "a64_emit.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ------------------------------------------------------------ flat memory */
#define RAM_SIZE (1u << 20)
static uint8_t g_ram[RAM_SIZE];
static uint32_t m_r32(void *c, uint32_t a){ (void)c; uint32_t v; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],4); return v; }
static uint16_t m_r16(void *c, uint32_t a){ (void)c; uint16_t v; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],2); return v; }
static uint8_t  m_r8 (void *c, uint32_t a){ (void)c; return g_ram[a&(RAM_SIZE-1)]; }
static void m_w32(void *c, uint32_t a, uint32_t v){ (void)c; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,4); }
static void m_w16(void *c, uint32_t a, uint16_t v){ (void)c; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,2); }
static void m_w8 (void *c, uint32_t a, uint8_t  v){ (void)c; g_ram[a&(RAM_SIZE-1)]=v; }
static const arm_bus_t g_bus = { NULL, m_r32, m_r16, m_r8, m_w32, m_w16, m_w8 };

#define CODE_WORDS 4096
static uint32_t g_code[CODE_WORDS];

static void load(arm_cpu_t *c, uint32_t at, const uint32_t *prog, size_t n) {
    size_t i;
    memset(g_ram, 0, sizeof g_ram);
    for (i = 0; i < n; i++) m_w32(NULL, at + (uint32_t)(i * 4), prog[i]);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~0x1fu) | ARM_MODE_SYS;   /* privileged, MMU off */
}

/* Translate a program loaded at `at`, starting from `at`. */
static bool xlate(arm_cpu_t *c, uint32_t at, const uint32_t *prog, size_t n,
                  jit_block_t *blk, size_t cap) {
    load(c, at, prog, n);
    memset(g_code, 0, sizeof g_code);
    return jit_translate(c, at, g_code, cap, blk);
}

/* How many times `w` appears in the emitted code. */
static unsigned count_word(const jit_block_t *b, uint32_t w) {
    unsigned i, n = 0;
    for (i = 0; i < b->code_words; i++) if (b->code[i] == w) n++;
    return n;
}
static void dump(const jit_block_t *b) {
    unsigned i;
    printf("    emitted %u words:", (unsigned)b->code_words);
    for (i = 0; i < b->code_words; i++) {
        if (i % 8 == 0) printf("\n     %3u:", i);
        printf(" %08x", b->code[i]);
    }
    printf("\n");
}

/* =========================================================== block shape */

static void test_block_ends_at_branch(void) {
    /* MOV r0,#1 ; ADD r1,r0,#2 ; B .-8 */
    uint32_t p[] = { 0xe3a00001, 0xe2801002, 0xeafffffc };
    arm_cpu_t c; jit_block_t b;
    bool ok = xlate(&c, 0, p, 3, &b, CODE_WORDS);
    CHECK(ok, "translated");
    CHECK(b.insn_count == 3, "insn_count=%u expect 3", b.insn_count);
    CHECK(b.native_count == 3, "native_count=%u expect 3", b.native_count);
    CHECK(b.end_reason == JIT_END_BRANCH, "end_reason=%d expect BRANCH", (int)b.end_reason);
    CHECK(b.key.va == 0 && b.key.pa == 0, "key va/pa");
    CHECK((b.key.flags & JIT_BLK_PRIV) != 0, "privileged flag set");
    CHECK((b.key.flags & JIT_BLK_THUMB) == 0, "thumb flag clear");
}

static void test_block_ends_before_unhandled(void) {
    /* MOV r0,#1 ; ADD r1,r0,#2 ; LDMIA sp!,{r0} ; MOV r2,#3
     * LDM is not translated, so the block must stop *before* it with two
     * instructions covered, and the runtime interprets the LDM. */
    uint32_t p[] = { 0xe3a00001, 0xe2801002, 0xe8bd0001, 0xe3a02003 };
    arm_cpu_t c; jit_block_t b;
    bool ok = xlate(&c, 0, p, 4, &b, CODE_WORDS);
    CHECK(ok, "translated");
    CHECK(b.insn_count == 2, "insn_count=%u expect 2", b.insn_count);
    CHECK(b.end_reason == JIT_END_FALLBACK, "end_reason=%d expect FALLBACK", (int)b.end_reason);
    /* The exit must ask the runtime to interpret, and resume at the LDM. */
    CHECK(count_word(&b, 0x52800020u) == 1, "movz w0,#JIT_EXIT_INTERPRET emitted");
    CHECK(count_word(&b, 0x52800108u) == 1, "movz w8,#8 (resume PC) emitted");
}

static void test_first_instruction_unhandled(void) {
    /* SWI #0 first: nothing to translate at all. */
    uint32_t p[] = { 0xef000000, 0xe3a00001 };
    arm_cpu_t c; jit_block_t b;
    bool ok = xlate(&c, 0, p, 2, &b, CODE_WORDS);
    CHECK(!ok, "translation declined");
    CHECK(b.insn_count == 0, "insn_count=%u expect 0", b.insn_count);
    CHECK(b.end_reason == JIT_END_FALLBACK, "end_reason=%d expect FALLBACK", (int)b.end_reason);
}

static void test_block_ends_at_page_boundary(void) {
    /* Four instructions at 0xff0..0xffc, then a new 4 KB page. */
    uint32_t p[] = { 0xe1a00000, 0xe1a00000, 0xe1a00000, 0xe1a00000, 0xe1a00000 };
    arm_cpu_t c; jit_block_t b;
    bool ok = xlate(&c, 0xff0, p, 5, &b, CODE_WORDS);
    CHECK(ok, "translated");
    CHECK(b.insn_count == 4, "insn_count=%u expect 4", b.insn_count);
    CHECK(b.end_reason == JIT_END_PAGE, "end_reason=%d expect PAGE", (int)b.end_reason);
}

static void test_block_length_cap(void) {
    uint32_t p[80];
    arm_cpu_t c; jit_block_t b;
    unsigned i;
    for (i = 0; i < 80; i++) p[i] = 0xe1a00000;   /* MOV r0,r0 */
    CHECK(xlate(&c, 0, p, 80, &b, CODE_WORDS), "translated");
    CHECK(b.insn_count == JIT_MAX_INSNS, "insn_count=%u expect %u", b.insn_count, JIT_MAX_INSNS);
    CHECK(b.end_reason == JIT_END_LIMIT, "end_reason=%d expect LIMIT", (int)b.end_reason);
}

static void test_code_buffer_exhaustion(void) {
    uint32_t p[80];
    arm_cpu_t c; jit_block_t b;
    unsigned i;
    for (i = 0; i < 80; i++) p[i] = 0xe1a00000;
    CHECK(xlate(&c, 0, p, 80, &b, 120), "translated into a small buffer");
    CHECK(b.insn_count > 0 && b.insn_count < JIT_MAX_INSNS,
          "insn_count=%u should be a partial block", b.insn_count);
    CHECK(b.end_reason == JIT_END_CODE_FULL, "end_reason=%d expect CODE_FULL", (int)b.end_reason);
    CHECK(b.code_words <= 120, "stayed inside the buffer (%u words)", (unsigned)b.code_words);
}

static void test_thumb_is_not_translated(void) {
    uint32_t p[] = { 0xe3a00001 };
    arm_cpu_t c; jit_block_t b;
    load(&c, 0, p, 1);
    c.cpsr |= ARM_CPSR_T;
    CHECK(!jit_translate(&c, 0, g_code, CODE_WORDS, &b), "thumb declined");
    CHECK((b.key.flags & JIT_BLK_THUMB) != 0, "thumb flag recorded");
    CHECK(b.insn_count == 0, "nothing translated");
}

/* ==================================== the interpreter fallback is total == */

static void test_deny_all_translates_nothing(void) {
    /* The debugging escape hatch of docs/dynarec.md §2: with every class
     * denied the JIT must decline everything, so the boot runs exactly as it
     * does today. */
    uint32_t p[] = { 0xe3a00001, 0xe5901000, 0xeafffffe };
    arm_cpu_t c; jit_block_t b;
    unsigned i;
    jit_set_deny(JIT_DENY_ALL);
    for (i = 0; i < 3; i++) {
        CHECK(!xlate(&c, 0, &p[i], 1, &b, CODE_WORDS), "class %u declined", i);
        CHECK(b.insn_count == 0, "class %u translated nothing", i);
    }
    jit_set_deny(0);
    CHECK(jit_get_deny() == 0, "deny mask cleared");
}

static void test_deny_one_class(void) {
    uint32_t p[] = { 0xe3a00001, 0xe5901000, 0xe3a02003, 0xeafffffe };
    arm_cpu_t c; jit_block_t b;
    jit_set_deny(JIT_DENY_LDST);
    CHECK(xlate(&c, 0, p, 4, &b, CODE_WORDS), "translated");
    CHECK(b.insn_count == 1, "insn_count=%u expect 1 (LDR denied)", b.insn_count);
    jit_set_deny(0);
    CHECK(xlate(&c, 0, p, 4, &b, CODE_WORDS), "translated with LDR allowed");
    CHECK(b.insn_count == 4, "insn_count=%u expect 4", b.insn_count);
}

/*
 * Every encoding the translator refuses. Each of these must produce a
 * zero-instruction block, because "not implemented" has to mean "interpreter"
 * and never "close enough".
 */
static void test_refused_encodings(void) {
    static const struct { uint32_t insn; const char *what; } CASES[] = {
        { 0xe0800110u, "ADD r0,r0,r0,lsl r1  (register-specified shift)" },
        { 0xe1b00060u, "MOVS r0,r0,rrx" },
        { 0xe1b00020u, "MOVS r0,r0,lsr #32" },
        { 0xe1b00040u, "MOVS r0,r0,asr #32" },
        { 0xe0100201u, "ANDS r0,r0,r1,lsl #4  (unknown shifter carry-out)" },
        { 0xe0610202u, "RSB  r0,r1,r2,lsl #4  (A64 shifts Rm, not Rn)" },
        { 0xe0a10202u, "ADC  r0,r1,r2,lsl #4  (A64 ADC takes no shift)" },
        { 0xe1a0f000u, "MOV pc,r0" },
        { 0xe28f0004u, "ADD r0,pc,#4" },
        { 0xe12fff10u, "BX r0" },
        { 0xe10f0000u, "MRS r0,cpsr" },
        { 0xe129f000u, "MSR cpsr,r0" },
        { 0xe0000091u, "MUL r0,r1,r0" },
        { 0xe1900f9fu, "LDREX r0,[r0]" },
        { 0xe1000090u, "SWP r0,r0,[r0]" },
        { 0xe8bd0001u, "LDMIA sp!,{r0}" },
        { 0xe5b01004u, "LDR r1,[r0,#4]!  (writeback)" },
        { 0xe4901000u, "LDR r1,[r0],#0   (post-index)" },
        { 0xe7901002u, "LDR r1,[r0,r2]   (register offset)" },
        { 0xe59ff000u, "LDR pc,[pc]" },
        { 0xe1c010b0u, "STRH r1,[r0]" },
        { 0xee010f10u, "MCR p15,..." },
        { 0xef000000u, "SWI #0" },
        { 0xf57ff01fu, "CLREX (unconditional space)" },
        { 0xf5d0f000u, "PLD [r0]  (unconditional space)" },
        { 0xe6bf0070u, "SXTB r0,r0 (media space)" },
    };
    arm_cpu_t c; jit_block_t b;
    unsigned i;
    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        bool ok = xlate(&c, 0, &CASES[i].insn, 1, &b, CODE_WORDS);
        CHECK(!ok && b.insn_count == 0, "%s must fall back", CASES[i].what);
    }
}

/*
 * The trap the interpreter documents at arm_interp.c:1616 — an immediate
 * data-processing instruction whose imm8 happens to have bits 7 and 4 set
 * looks like the multiply/extension space unless bit 25 is checked. MOV
 * r0,#0x90 must still be translated as a MOV.
 */
static void test_imm_dp_not_mistaken_for_multiply(void) {
    uint32_t p[] = { 0xe3a00090, 0xeafffffe };  /* MOV r0,#0x90 ; B . */
    arm_cpu_t c; jit_block_t b;
    CHECK(xlate(&c, 0, p, 2, &b, CODE_WORDS), "translated");
    CHECK(b.insn_count == 2, "insn_count=%u expect 2", b.insn_count);
    CHECK(count_word(&b, 0x52801213u) == 1, "movz w19,#0x90 emitted");
}

/* ======================================================== emitted code === */

/*
 * The prologue is fixed, load-bearing and identical in every block: it saves
 * the callee-saved registers it is about to fill with guest state, takes the
 * arm_cpu_t * from x0, and loads r0-r7, r13 and the flags. Byte-exact.
 */
static void test_prologue_is_exact(void) {
    static const uint32_t WANT[] = {
        0xa9b97bfd,   /* stp  x29, x30, [sp, #-112]! */
        0x910003fd,   /* mov  x29, sp                */
        0xa90153f3,   /* stp  x19, x20, [sp, #16]    */
        0xa9025bf5,   /* stp  x21, x22, [sp, #32]    */
        0xa90363f7,   /* stp  x23, x24, [sp, #48]    */
        0xa9046bf9,   /* stp  x25, x26, [sp, #64]    */
        0xa90573fb,   /* stp  x27, x28, [sp, #80]    */
        0xaa0003fc,   /* mov  x28, x0                */
        0x29405393,   /* ldp  w19, w20, [x28]        */
        0x29415b95,   /* ldp  w21, w22, [x28, #8]    */
        0x29426397,   /* ldp  w23, w24, [x28, #16]   */
        0x29436b99,   /* ldp  w25, w26, [x28, #24]   */
        0xb940379b,   /* ldr  w27, [x28, #52]        */
        0xb9404389,   /* ldr  w9,  [x28, #64]        */
        0xd51b4209    /* msr  nzcv, x9               */
    };
    uint32_t p[] = { 0xeafffffe };   /* B . */
    arm_cpu_t c; jit_block_t b;
    unsigned i, bad = 0;
    CHECK(xlate(&c, 0, p, 1, &b, CODE_WORDS), "translated");
    for (i = 0; i < sizeof WANT / sizeof WANT[0]; i++)
        if (i >= b.code_words || b.code[i] != WANT[i]) bad++;
    CHECK(bad == 0, "prologue mismatched in %u of %u words", bad,
          (unsigned)(sizeof WANT / sizeof WANT[0]));
    if (bad) dump(&b);
}

/* One guest instruction, one host instruction, in the pinned registers. */
static void test_dp_bodies_are_exact(void) {
    static const struct { uint32_t guest; uint32_t host; const char *what; } CASES[] = {
        { 0xe3a0002au, 0x52800553u, "MOV r0,#42     -> movz w19,#42"       },
        { 0xe2900001u, 0x31000673u, "ADDS r0,r0,#1  -> adds w19,w19,#1"    },
        { 0xe2400001u, 0x51000673u, "SUB  r0,r0,#1  -> sub  w19,w19,#1"    },
        { 0xe3500005u, 0x7100167fu, "CMP  r0,#5     -> cmp  w19,#5"        },
        { 0xe20000ffu, 0x12001e73u, "AND  r0,r0,#255-> and  w19,w19,#0xff" },
        { 0xe0812002u, 0x0b150295u, "ADD  r2,r1,r2  -> add  w21,w20,w21"   },
        { 0xe1a00001u, 0x2a1403f3u, "MOV  r0,r1     -> mov  w19,w20"       },
        { 0xe1e00001u, 0x2a3403f3u, "MVN  r0,r1     -> mvn  w19,w20"       },
        { 0xe1a00181u, 0x2a140ff3u, "MOV  r0,r1,lsl#3 -> orr w19,wzr,w20,lsl#3" },
        { 0xe0a12003u, 0x1a160295u, "ADC  r2,r1,r3  -> adc  w21,w20,w22"   }
    };
    arm_cpu_t c; jit_block_t b;
    unsigned i;
    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        uint32_t p[2]; p[0] = CASES[i].guest; p[1] = 0xeafffffe;
        if (!xlate(&c, 0, p, 2, &b, CODE_WORDS)) { g_fail++; printf("  FAIL %s: not translated\n", CASES[i].what); continue; }
        if (count_word(&b, CASES[i].host) != 1) {
            g_fail++; printf("  FAIL %s: expected host word %08x once\n", CASES[i].what, CASES[i].host);
            dump(&b);
        } else g_pass++;
    }
}

/*
 * A32's logical operations set C from the barrel shifter and PRESERVE V;
 * A64's ANDS forces both to zero. The fixup of docs/dynarec.md §5.2(1) must
 * therefore appear for ANDS/TST/MOVS and must NOT appear for ADDS/SUBS, whose
 * flags map exactly.
 */
static void test_logical_flag_fixup(void) {
    arm_cpu_t c; jit_block_t b;
    uint32_t p[2]; p[1] = 0xeafffffe;

    p[0] = 0xe21000ffu;                      /* ANDS r0,r0,#255 (rot 0) */
    CHECK(xlate(&c, 0, p, 2, &b, CODE_WORDS), "ANDS translated");
    CHECK(count_word(&b, 0x6a13027fu) == 1, "ands wzr,w19,w19 recomputes N,Z");
    CHECK(count_word(&b, 0xd35c7529u) == 1, "ubfx x9,x9,#28,#2 saves old C,V");
    CHECK(count_word(&b, 0xb364052au) == 1, "bfi  x10,x9,#28,#2 restores C,V");

    p[0] = 0xe2900001u;                      /* ADDS r0,r0,#1 */
    CHECK(xlate(&c, 0, p, 2, &b, CODE_WORDS), "ADDS translated");
    CHECK(count_word(&b, 0x6a13027fu) == 0, "ADDS needs no logical fixup");

    /* TST always sets flags even though S is encoded as 1 anyway. */
    p[0] = 0xe3100001u;                      /* TST r0,#1 */
    CHECK(xlate(&c, 0, p, 2, &b, CODE_WORDS), "TST translated");
    CHECK(count_word(&b, 0x6a0b017fu) == 1, "ands wzr,w11,w11 from the scratch result");

    /*
     * A rotated immediate makes the shifter carry-out a translate-time
     * constant (§5.2(4)); C is then forced rather than preserved. ANDS
     * r0,r0,#0x80000000 is imm8 = 0x02 rotated right by 2, so C := 1.
     */
    p[0] = 0xe2100102u;                      /* ANDS r0,r0,#0x80000000 */
    CHECK(xlate(&c, 0, p, 2, &b, CODE_WORDS), "ANDS rot!=0 translated");
    CHECK(count_word(&b, 0xd35c7529u) == 0, "rotated form does not preserve C");
    CHECK(count_word(&b, 0xb263014au) == 1, "orr x10,x10,#0x20000000 forces C=1");
}

/* Conditional execution is a single B.<inverse> over the body (§5.1). */
static void test_conditional_is_one_branch(void) {
    /* ADDEQ r0,r0,#1 ; B . */
    uint32_t p[] = { 0x02800001, 0xeafffffe };
    arm_cpu_t c; jit_block_t b;
    CHECK(xlate(&c, 0, p, 2, &b, CODE_WORDS), "translated");
    CHECK(b.insn_count == 2, "insn_count=%u expect 2", b.insn_count);
    CHECK(count_word(&b, 0x54000041u) == 1, "b.ne +2 skips the one-word body");
    CHECK(count_word(&b, 0x11000673u) == 1, "add w19,w19,#1 is the body");
}

/*
 * A conditional branch produces two exits: the taken target and the fall
 * through. Chaining will later patch both edges independently (§3.5).
 */
static void test_conditional_branch_has_two_exits(void) {
    /* BEQ .+8 (target 0x10 from PC 0) ; ... */
    uint32_t p[] = { 0x0a000002, 0xe1a00000, 0xe1a00000, 0xe1a00000 };
    arm_cpu_t c; jit_block_t b;
    CHECK(xlate(&c, 0, p, 4, &b, CODE_WORDS), "translated");
    CHECK(b.insn_count == 1, "insn_count=%u expect 1", b.insn_count);
    CHECK(b.end_reason == JIT_END_BRANCH, "ends at the branch");
    CHECK(count_word(&b, 0x52800208u) == 1, "movz w8,#0x10 (taken target)");
    CHECK(count_word(&b, 0x52800088u) == 1, "movz w8,#4    (fall-through)");
}

static void test_bl_writes_lr(void) {
    /* BL .+4 from 0: LR = 4, target = 0x10 */
    uint32_t p[] = { 0xeb000002 };
    arm_cpu_t c; jit_block_t b;
    CHECK(xlate(&c, 0, p, 1, &b, CODE_WORDS), "translated");
    CHECK(count_word(&b, 0x52800089u) == 1, "movz w9,#4 (return address)");
    CHECK(count_word(&b, 0xb9003b89u) == 1, "str w9,[x28,#56] (cpu->r[14])");
}

/*
 * A load must (a) publish the guest PC before it can fault, because the
 * interpreter re-executes the instruction and LR_abt is pc + 8, (b) call the
 * helper indirectly since a BL cannot reach an arbitrary host address, and
 * (c) test the fault bit before writing the destination.
 */
static void test_load_sequence(void) {
    /* LDR r1,[r0,#4] ; B . */
    uint32_t p[] = { 0xe5901004, 0xeafffffe };
    arm_cpu_t c; jit_block_t b;
    CHECK(xlate(&c, 0, p, 2, &b, CODE_WORDS), "translated");
    /* Twice: once here, once in the epilogue that publishes the resume PC. */
    CHECK(count_word(&b, 0xb9003f88u) == 2, "str w8,[x28,#60] publishes cpu->r[15]");
    CHECK(count_word(&b, 0x11001261u) == 1, "add w1,w19,#4 computes the address");
    CHECK(count_word(&b, 0xaa1c03e0u) == 1, "mov x0,x28 passes the cpu");
    CHECK(count_word(&b, 0xd63f0200u) == 1, "blr x16 calls the helper");
    CHECK(count_word(&b, 0x2a0003f4u) == 1, "mov w20,w0 commits r1 after the check");
    /* NZCV must be saved across the C call (§4.4). */
    CHECK(count_word(&b, 0xd53b4209u) >= 1, "mrs x9,nzcv before the call");
    CHECK(count_word(&b, 0xf90033e9u) == 1, "str x9,[sp,#96] parks the flags");
    CHECK(count_word(&b, 0xf94033e9u) == 1, "ldr x9,[sp,#96] restores them");
}

static void test_store_sequence(void) {
    /* STRB r1,[r0,#4] ; B . */
    uint32_t p[] = { 0xe5c01004, 0xeafffffe };
    arm_cpu_t c; jit_block_t b;
    CHECK(xlate(&c, 0, p, 2, &b, CODE_WORDS), "translated");
    CHECK(count_word(&b, 0x2a1403e2u) == 1, "mov w2,w20 passes the stored value");
    CHECK(count_word(&b, 0x52800040u) == 1, "movz w0,#JIT_EXIT_ABORT on the fault path");
}

/* ========================================================= code buffers == */

static void test_code_buffer(void) {
    jit_buf_t buf;
    uint32_t *a, *b2;
    CHECK(jit_buf_alloc(&buf, 1024), "arena allocated");
    CHECK(buf.size >= 1024 && (buf.size % 0x4000u) == 0, "arena rounded to 16 KB");
    CHECK(jit_buf_policy(&buf) != NULL, "policy: %s", jit_buf_policy(&buf));
    a = jit_buf_take(&buf, 3);
    b2 = jit_buf_take(&buf, 3);
    CHECK(a != NULL && b2 != NULL, "two allocations");
    CHECK((((uintptr_t)a) & 15u) == 0 && (((uintptr_t)b2) & 15u) == 0, "16-byte aligned");
    CHECK(b2 >= a + 3, "allocations do not overlap");
    CHECK(jit_buf_take(&buf, buf.size) == NULL, "over-large request refused");
    jit_buf_begin_write(&buf);
    a[0] = 0xd503201fu;
    jit_buf_end_write(&buf);
    jit_buf_commit(&buf, a, 4);
    jit_buf_free(&buf);
    CHECK(buf.base == NULL, "freed");
    CHECK(jit_buf_take(&buf, 1) == NULL, "no allocation from a freed arena");
    /* On the x86 dev box this is false, and jit_enter must be a no-op. */
    if (!jit_host_can_execute()) {
        jit_block_t blk;
        memset(&blk, 0, sizeof blk);
        CHECK(jit_enter(&blk, NULL) == JIT_EXIT_INTERPRET,
              "jit_enter falls back where arm64 cannot run");
    }
}

int main(void) {
    printf("jit translator (host can execute arm64: %s)\n",
           jit_host_can_execute() ? "yes" : "no");
    test_block_ends_at_branch();
    test_block_ends_before_unhandled();
    test_first_instruction_unhandled();
    test_block_ends_at_page_boundary();
    test_block_length_cap();
    test_code_buffer_exhaustion();
    test_thumb_is_not_translated();
    test_deny_all_translates_nothing();
    test_deny_one_class();
    test_refused_encodings();
    test_imm_dp_not_mistaken_for_multiply();
    test_prologue_is_exact();
    test_dp_bodies_are_exact();
    test_logical_flag_fixup();
    test_conditional_is_one_branch();
    test_conditional_branch_has_two_exits();
    test_bl_writes_lr();
    test_load_sequence();
    test_store_sequence();
    test_code_buffer();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

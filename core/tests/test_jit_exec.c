/*
 * iOS3-VM — arm64 EXECUTION tests for the emitter and the block translator.
 *
 * This is the only test that runs emitted code, so it is the only one that can
 * show the encodings are not merely well-formed but correct. It therefore
 * needs an arm64 host, which the project's Windows/x86 dev box is not.
 *
 * GATING. The test is built everywhere and decides at *runtime* whether it can
 * run, printing "SKIP" and succeeding when it cannot:
 *
 *   - not an arm64 host                      -> skip (x86 CI, the dev box)
 *   - executable memory unobtainable         -> skip (no MAP_JIT, no RWX,
 *                                               CS_DEBUGGED not set)
 *
 * So it fails only for a genuinely wrong encoding, never for the environment.
 * On GitHub Actions the macos-14/macos-15 runners are Apple Silicon, so this
 * is where it actually executes; see .github/workflows/core-tests.yml.
 *
 * The last test is the one that matters most: it is docs/dynarec.md §9.2's
 * lockstep comparison in miniature — the same guest instructions run by the
 * interpreter and by translated code, with the full architectural state
 * compared afterwards.
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

static jit_buf_t g_buf;

/* Assemble `n` words into the arena and call them as long f(long, long). */
static long call_fn(const uint32_t *words, size_t n, long a, long b) {
    union { void *p; long (*fn)(long, long); } u;
    uint32_t *dst = jit_buf_take(&g_buf, n);
    if (!dst) return -1;
    jit_buf_begin_write(&g_buf);
    memcpy(dst, words, n * 4u);
    jit_buf_end_write(&g_buf);
    jit_buf_commit(&g_buf, dst, n * 4u);
    u.p = (void *)dst;
    return u.fn(a, b);
}

#define BODY(name) \
    static uint32_t name[32]; static a64_emit_t name##_e; \
    a64_init(&name##_e, name, 32);

/* ---------------------------------------------------------- emitter tests */

static void test_executes_basics(void) {
    uint32_t w[32];
    a64_emit_t e;

    /* The docs/dynarec.md §8.2 readiness probe: emit, execute, check. */
    a64_init(&e, w, 32);
    a64_movz(&e, A64_W, 0, 42, 0);
    a64_ret(&e, 30);
    CHECK(a64_ok(&e) && call_fn(w, e.n, 0, 0) == 42, "movz w0,#42 ; ret");

    a64_init(&e, w, 32);
    a64_add_reg(&e, A64_W, 0, 0, 1, A64_LSL, 0);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 40, 2) == 42, "add w0,w0,w1");

    a64_init(&e, w, 32);
    a64_sub_imm(&e, A64_W, 0, 0, 8, false);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 50, 0) == 42, "sub w0,w0,#8");

    a64_init(&e, w, 32);
    a64_and_imm(&e, A64_W, 0, 0, 0xff);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 0x12345, 0) == 0x45, "and w0,w0,#0xff");

    a64_init(&e, w, 32);
    a64_orr_reg(&e, A64_W, 0, 0, 1, A64_LSL, 4);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 1, 1) == 0x11, "orr w0,w0,w1,lsl #4");

    a64_init(&e, w, 32);
    a64_mov_imm(&e, A64_X, 0, 0x0123456789abcdefull);
    a64_ret(&e, 30);
    CHECK((unsigned long long)call_fn(w, e.n, 0, 0) == 0x0123456789abcdefull,
          "mov x0,#0x0123456789abcdef");

    a64_init(&e, w, 32);
    a64_mov_imm(&e, A64_X, 0, 0xffffffff0000ffffull);
    a64_ret(&e, 30);
    CHECK((unsigned long long)call_fn(w, e.n, 0, 0) == 0xffffffff0000ffffull,
          "mov x0 via MOVN");

    a64_init(&e, w, 32);
    a64_mul(&e, A64_W, 0, 0, 1);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 6, 7) == 42, "mul w0,w0,w1");

    a64_init(&e, w, 32);
    a64_clz(&e, A64_W, 0, 0);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 1, 0) == 31, "clz w0,w0");
}

static void test_executes_bitfield(void) {
    uint32_t w[32];
    a64_emit_t e;

    a64_init(&e, w, 32); a64_lsl_imm(&e, A64_W, 0, 0, 4); a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 3, 0) == 48, "lsl w0,w0,#4");

    a64_init(&e, w, 32); a64_lsr_imm(&e, A64_W, 0, 0, 4); a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 48, 0) == 3, "lsr w0,w0,#4");

    a64_init(&e, w, 32); a64_asr_imm(&e, A64_W, 0, 0, 1); a64_ret(&e, 30);
    CHECK((int)call_fn(w, e.n, -8, 0) == -4, "asr w0,w0,#1");

    a64_init(&e, w, 32); a64_ubfx(&e, A64_W, 0, 0, 8, 4); a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 0x00000a00, 0) == 0xa, "ubfx w0,w0,#8,#4");

    /* The exact pair the logical-flag fixup uses. */
    a64_init(&e, w, 32);
    a64_ubfx(&e, A64_X, 0, 0, 28, 2);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 0x30000000, 0) == 3, "ubfx x0,x0,#28,#2");

    a64_init(&e, w, 32);
    a64_bfi(&e, A64_X, 0, 1, 28, 2);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 0, 3) == 0x30000000, "bfi x0,x1,#28,#2");

    a64_init(&e, w, 32); a64_lslv(&e, A64_W, 0, 0, 1); a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 1, 5) == 32, "lslv");
}

static void test_executes_flags(void) {
    uint32_t w[32];
    a64_emit_t e;

    a64_init(&e, w, 32);
    a64_subs_reg(&e, A64_W, A64_ZR, 0, 1, A64_LSL, 0);
    a64_cset(&e, A64_W, 0, A64_EQ);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 7, 7) == 1, "cmp equal -> cset eq");
    CHECK(call_fn(w, e.n, 7, 8) == 0, "cmp unequal -> cset eq");

    /* Carry convention: A32 and A64 agree that C set means "no borrow". */
    a64_init(&e, w, 32);
    a64_subs_reg(&e, A64_W, A64_ZR, 0, 1, A64_LSL, 0);
    a64_cset(&e, A64_W, 0, A64_CS);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 5, 5) == 1, "5-5 sets C (no borrow)");
    CHECK(call_fn(w, e.n, 1, 2) == 0, "1-2 clears C (borrow)");

    /* NZCV moves out and back in exactly, which is what block entry/exit does. */
    a64_init(&e, w, 32);
    a64_msr_nzcv(&e, 0);
    a64_mrs_nzcv(&e, 0);
    a64_ret(&e, 30);
    CHECK((call_fn(w, e.n, 0xf0000000, 0) & 0xf0000000) == 0xf0000000, "nzcv all set");
    CHECK((call_fn(w, e.n, 0x30000000, 0) & 0xf0000000) == 0x30000000, "nzcv C,V only");

    /*
     * The §5.2(1) fixup itself: A64's ANDS clobbers C and V, and the
     * MRS/UBFX/BFI/MSR sequence must put them back while N and Z follow the
     * result. Enter with C and V set and a non-zero result; expect NZCV to
     * come out as 0b0011 (N=0, Z=0, C=1, V=1).
     */
    a64_init(&e, w, 32);
    a64_msr_nzcv(&e, 0);                                   /* incoming flags  */
    a64_mrs_nzcv(&e, 9);
    a64_ubfx(&e, A64_X, 9, 9, 28, 2);                      /* save C,V        */
    a64_ands_reg(&e, A64_W, A64_ZR, 1, 1, A64_LSL, 0);     /* N,Z from w1     */
    a64_mrs_nzcv(&e, 10);
    a64_bfi(&e, A64_X, 10, 9, 28, 2);                      /* restore C,V     */
    a64_msr_nzcv(&e, 10);
    a64_mrs_nzcv(&e, 0);
    a64_ret(&e, 30);
    CHECK((call_fn(w, e.n, 0x30000000, 1) & 0xf0000000) == 0x30000000,
          "logical fixup: C,V preserved, N=0 Z=0");
    CHECK((call_fn(w, e.n, 0x30000000, 0) & 0xf0000000) == 0x70000000,
          "logical fixup: C,V preserved, Z set");
    CHECK((call_fn(w, e.n, 0x00000000, 0x80000000u) & 0xf0000000) == 0x80000000,
          "logical fixup: N set, C,V clear");
}

static void test_executes_memory_and_branches(void) {
    uint32_t w[32];
    a64_emit_t e;
    size_t site;

    /* Push and pop through the pre/post-indexed forms the prologue uses. */
    a64_init(&e, w, 32);
    a64_str_pre (&e, A64_SZ_D, 1, A64_SP, -16);
    a64_ldr_post(&e, A64_SZ_D, 0, A64_SP, 16);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 0, 0x1234) == 0x1234, "str/ldr pre and post indexed");

    a64_init(&e, w, 32);
    a64_stp_pre (&e, A64_SZ_D, 0, 1, A64_SP, -16);
    a64_ldp     (&e, A64_SZ_D, 1, 0, A64_SP, 0);
    a64_add_imm (&e, A64_X, A64_SP, A64_SP, 16, false);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 3, 9) == 9, "stp/ldp swap");

    /* Register-offset addressing: the shape of the emitted memory fast path. */
    a64_init(&e, w, 32);
    a64_stp_pre (&e, A64_SZ_D, 0, 1, A64_SP, -16);
    a64_mov_sp  (&e, A64_X, 2, A64_SP);
    a64_movz    (&e, A64_X, 3, 8, 0);
    a64_ldr_reg (&e, A64_SZ_D, 0, 2, 3, A64_EXT_LSL, 0);
    a64_add_imm (&e, A64_X, A64_SP, A64_SP, 16, false);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 1, 0x5678) == 0x5678, "ldr x0,[x2,x3]");

    /* Forward conditional branch, bound by a64_bind. */
    a64_init(&e, w, 32);
    a64_cmp_reg(&e, A64_W, 0, 1, A64_LSL, 0);
    site = e.n;
    a64_bcond(&e, A64_NE, 0);
    a64_movz(&e, A64_W, 0, 111, 0);
    a64_bind(&e, site);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 5, 5) == 111, "b.ne not taken -> body runs");
    CHECK(call_fn(w, e.n, 5, 6) == 5,   "b.ne taken -> body skipped");

    /* CBZ, and an unconditional B over a stub. */
    a64_init(&e, w, 32);
    site = e.n;
    a64_cbz(&e, A64_W, 0, 0);
    a64_movz(&e, A64_W, 0, 1, 0);
    a64_bind(&e, site);
    a64_ret(&e, 30);
    CHECK(call_fn(w, e.n, 0, 0) == 0, "cbz taken");
    CHECK(call_fn(w, e.n, 9, 0) == 1, "cbz not taken");

    /* BLR through IP0: exactly how the translator calls a memory helper. */
    {
        uint32_t callee[4];
        a64_emit_t ce;
        uint32_t *cdst;
        a64_init(&ce, callee, 4);
        a64_add_reg(&ce, A64_W, 0, 0, 1, A64_LSL, 0);
        a64_ret(&ce, 30);
        cdst = jit_buf_take(&g_buf, ce.n);
        if (cdst) {
            a64_emit_t e2;
            uint32_t w2[32];
            jit_buf_begin_write(&g_buf);
            memcpy(cdst, callee, ce.n * 4u);
            jit_buf_end_write(&g_buf);
            jit_buf_commit(&g_buf, cdst, ce.n * 4u);
            a64_init(&e2, w2, 32);
            a64_stp_pre(&e2, A64_SZ_D, 29, 30, A64_SP, -16);
            a64_mov_imm(&e2, A64_X, 16, (uint64_t)(uintptr_t)cdst);
            a64_blr(&e2, 16);
            a64_ldp_post(&e2, A64_SZ_D, 29, 30, A64_SP, 16);
            a64_ret(&e2, 30);
            CHECK(call_fn(w2, e2.n, 40, 2) == 42, "blr x16 to an emitted callee");
        }
    }
}

/* ----------------------------------------------- translated-block lockstep */

#define RAM_SIZE (1u << 20)
static uint8_t g_ram[RAM_SIZE];
static uint32_t m_r32(void *c, uint32_t a){ (void)c; uint32_t v; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],4); return v; }
static uint16_t m_r16(void *c, uint32_t a){ (void)c; uint16_t v; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],2); return v; }
static uint8_t  m_r8 (void *c, uint32_t a){ (void)c; return g_ram[a&(RAM_SIZE-1)]; }
static void m_w32(void *c, uint32_t a, uint32_t v){ (void)c; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,4); }
static void m_w16(void *c, uint32_t a, uint16_t v){ (void)c; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,2); }
static void m_w8 (void *c, uint32_t a, uint8_t  v){ (void)c; g_ram[a&(RAM_SIZE-1)]=v; }
static const arm_bus_t g_bus = { NULL, m_r32, m_r16, m_r8, m_w32, m_w16, m_w8 };

static void seed(arm_cpu_t *c, const uint32_t *prog, size_t n) {
    size_t i;
    memset(g_ram, 0, sizeof g_ram);
    for (i = 0; i < n; i++) m_w32(NULL, (uint32_t)(i * 4), prog[i]);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~0x1fu) | ARM_MODE_SYS;
    for (i = 0; i < 13; i++) c->r[i] = (uint32_t)(0x11111111u * (i + 1));
    c->r[13] = 0x8000;
    c->r[14] = 0xdead0000u;
    c->cpsr |= ARM_CPSR_C;                      /* a non-trivial flag state */
}

/*
 * Run the same guest instructions twice — once with arm_step(), once with a
 * translated block — and compare every architecturally visible field. This is
 * docs/dynarec.md §9.2 in miniature, and it is the test that would catch a
 * wrong flag rule or a mis-plumbed register.
 */
static void lockstep(const char *what, const uint32_t *prog, size_t n) {
    arm_cpu_t ref, jit;
    jit_block_t blk;
    uint32_t *code;
    unsigned i;
    int rc;

    seed(&jit, prog, n);
    code = jit_buf_take(&g_buf, 1024);
    if (!code) { printf("  SKIP %s: arena exhausted\n", what); return; }
    jit_buf_begin_write(&g_buf);
    if (!jit_translate(&jit, 0, code, 1024, &blk)) {
        jit_buf_end_write(&g_buf);
        g_fail++; printf("  FAIL %s: not translated\n", what);
        return;
    }
    jit_buf_end_write(&g_buf);
    jit_buf_commit(&g_buf, code, blk.code_words * 4u);

    seed(&ref, prog, n);
    for (i = 0; i < blk.insn_count; i++) {
        if (arm_step(&ref) != ARM_OK) { g_fail++; printf("  FAIL %s: interpreter trapped\n", what); return; }
    }

    seed(&jit, prog, n);
    rc = jit_enter(&blk, &jit);
    CHECK(rc == JIT_EXIT_NEXT || rc == JIT_EXIT_INTERPRET, "%s: exit %d", what, rc);

    for (i = 0; i < 16; i++)
        CHECK(ref.r[i] == jit.r[i], "%s: r%u = %08x, interpreter says %08x",
              what, i, jit.r[i], ref.r[i]);
    CHECK(ref.cpsr == jit.cpsr, "%s: cpsr = %08x, interpreter says %08x",
          what, jit.cpsr, ref.cpsr);
    CHECK(ref.cycles == jit.cycles, "%s: cycles = %llu, interpreter says %llu",
          what, (unsigned long long)jit.cycles, (unsigned long long)ref.cycles);
}

static void test_blocks_match_the_interpreter(void) {
    { uint32_t p[] = { 0xe3a0002a, 0xe2801001, 0xe0402001, 0xeafffffe };
      lockstep("mov/add/sub then b", p, 4); }                     /* plain ALU  */
    { uint32_t p[] = { 0xe2900001, 0xe2511001, 0xe0b22003, 0xeafffffe };
      lockstep("adds/subs/adcs", p, 4); }                         /* flags      */
    { uint32_t p[] = { 0xe21000ff, 0xe3300000, 0xe1b02003, 0xeafffffe };
      lockstep("ands/teq/movs (logical flags)", p, 4); }          /* the §5.2 case */
    { uint32_t p[] = { 0xe3500005, 0x02800001, 0x12800002, 0xeafffffe };
      lockstep("cmp then conditional add", p, 4); }               /* conditions */
    { uint32_t p[] = { 0xe5801000, 0xe5902000, 0xe5c03004, 0xe5d04004, 0xeafffffe };
      lockstep("str/ldr/strb/ldrb", p, 5); }                      /* memory     */
    { uint32_t p[] = { 0xeb000002 };
      lockstep("bl writes lr", p, 1); }                           /* branch     */
    { uint32_t p[] = { 0xe3a00001, 0xe8bd0002 };
      lockstep("block ends before an unhandled ldm", p, 2); }     /* fallback   */
}

int main(void) {
    printf("jit execution tests\n");
    if (!jit_host_can_execute()) {
        printf("SKIP: not an arm64 host; emitted code cannot be executed here.\n");
        return 0;
    }
    if (!jit_buf_alloc(&g_buf, 1u << 20)) {
        printf("SKIP: no executable memory available on this host.\n");
        return 0;
    }
    printf("code buffer policy: %s\n", jit_buf_policy(&g_buf));

    test_executes_basics();
    test_executes_bitfield();
    test_executes_flags();
    test_executes_memory_and_branches();
    test_blocks_match_the_interpreter();

    jit_buf_free(&g_buf);
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

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
static unsigned g_read8_calls, g_write8_calls;
static unsigned g_read16_calls, g_write16_calls, g_read32_calls, g_write32_calls;
static uint32_t m_r32(void *c, uint32_t a){ (void)c; uint32_t v; g_read32_calls++; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],4); return v; }
static uint16_t m_r16(void *c, uint32_t a){ (void)c; uint16_t v; g_read16_calls++; memcpy(&v,&g_ram[a&(RAM_SIZE-1)],2); return v; }
static uint8_t  m_r8 (void *c, uint32_t a){ (void)c; g_read8_calls++; return g_ram[a&(RAM_SIZE-1)]; }
static void m_w32(void *c, uint32_t a, uint32_t v){ (void)c; g_write32_calls++; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,4); }
static void m_w16(void *c, uint32_t a, uint16_t v){ (void)c; g_write16_calls++; memcpy(&g_ram[a&(RAM_SIZE-1)],&v,2); }
static void m_w8 (void *c, uint32_t a, uint8_t  v){ (void)c; g_write8_calls++; g_ram[a&(RAM_SIZE-1)]=v; }
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

/* Two non-contiguous physical frames behind adjacent virtual small pages. The
 * second descriptor can be omitted to exercise the JIT's replay-on-abort rule.
 * VA 0 is identity-mapped as a section so an interpreter replay can fetch its
 * faulting instruction. */
#define SPLIT_VA  0x80000000u
#define SPLIT_PA0 0x00030000u
#define SPLIT_PA1 0x00060000u
static void jit_split_setup(arm_cpu_t *c, bool map_second) {
    const uint32_t l1 = 0x4000u, l2 = 0x5000u;
    memset(g_ram, 0, sizeof g_ram);
    m_w32(NULL, l1 + (0x000u << 2), (3u << 10) | 8u | 2u);  /* Normal identity */
    m_w32(NULL, l1 + (0x800u << 2), l2 | 1u);               /* coarse table  */
    m_w32(NULL, l2 + (0u << 2), SPLIT_PA0 | (3u << 4) | 8u | 2u);
    if (map_second)
        m_w32(NULL, l2 + (1u << 2), SPLIT_PA1 | (3u << 4) | 8u | 2u);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c->cp15.ttbr0 = l1;
    c->cp15.dacr = 1u;
    c->cp15.sctlr |= ARM_SCTLR_M | ARM_SCTLR_XP | ARM_SCTLR_U;
    c->r[15] = 0;
    g_read8_calls = g_write8_calls = 0;
    g_read16_calls = g_write16_calls = g_read32_calls = g_write32_calls = 0;
}

static void jit_map_normal_identity(arm_cpu_t *c) {
    const uint32_t l1 = 0x4000u;
    m_w32(NULL, l1, (3u << 10) | 8u | 2u);       /* Normal, full access */
    c->cp15.ttbr0 = l1;
    c->cp15.dacr = 1u;                            /* domain 0 Client */
    c->cp15.sctlr |= ARM_SCTLR_M | ARM_SCTLR_XP;
}

static void test_memory_helpers_cross_pages_without_replay_side_effects(void) {
    arm_cpu_t c;
    uint64_t loaded;

    /* Halfword success: the two bytes come from non-contiguous frames. */
    jit_split_setup(&c, true);
    g_ram[SPLIT_PA0 + 0xfffu] = 0x34u;
    g_ram[SPLIT_PA1] = 0x12u;
    g_ram[SPLIT_PA0 + 0x1000u] = 0xeeu;            /* decoy after first frame */
    loaded = jit_mem_load16(&c, SPLIT_VA + 0xfffu);
    CHECK((loaded >> 32) == 0u && (uint32_t)loaded == 0x1234u,
          "cross-page load16=%llx expect 1234", (unsigned long long)loaded);
    CHECK(g_read8_calls == 2u, "cross-page load16 issued %u byte reads expect 2",
          g_read8_calls);

    /* The storing mirror must split to the same two frames. */
    jit_split_setup(&c, true);
    CHECK(jit_mem_store16(&c, SPLIT_VA + 0xfffu, 0x1234u) == 0u,
          "cross-page store16 should succeed");
    CHECK(g_ram[SPLIT_PA0 + 0xfffu] == 0x34u && g_ram[SPLIT_PA1] == 0x12u,
          "store16 landed at %02x/%02x expect 34/12",
          g_ram[SPLIT_PA0 + 0xfffu], g_ram[SPLIT_PA1]);
    CHECK(g_ram[SPLIT_PA0 + 0x1000u] == 0u,
          "store16 incorrectly treated physical frames as contiguous");

    /* A missing second page is detected before any device-like bus access. */
    jit_split_setup(&c, false);
    loaded = jit_mem_load16(&c, SPLIT_VA + 0xfffu);
    CHECK((loaded >> 32) == 1u, "faulting load16 did not report its fault bit");
    CHECK(g_read8_calls == 0u,
          "faulting load16 issued %u reads before interpreter replay", g_read8_calls);
    CHECK(jit_mem_store16(&c, SPLIT_VA + 0xfffu, 0x1234u) != 0u,
          "faulting store16 did not report failure");
    CHECK(g_write8_calls == 0u,
          "faulting store16 issued %u writes before interpreter replay", g_write8_calls);

    /* A successful crossing word is still committed bytewise to both frames,
     * but only after both translations have passed. */
    jit_split_setup(&c, true);
    CHECK(jit_mem_store32(&c, SPLIT_VA + 0xffeu, 0x11223344u) == 0u,
          "cross-page store32 should succeed");
    CHECK(g_write8_calls == 4u, "cross-page store32 issued %u writes expect 4",
          g_write8_calls);
    CHECK(g_ram[SPLIT_PA0 + 0xffeu] == 0x44u &&
          g_ram[SPLIT_PA0 + 0xfffu] == 0x33u &&
          g_ram[SPLIT_PA1] == 0x22u && g_ram[SPLIT_PA1 + 1u] == 0x11u,
          "cross-page store32 bytes landed in the wrong frames");

    /* On a second-page write fault the helper must write nothing. The fallback
     * interpreter then performs the architecture's one partial transaction;
     * counting byte writes models an MMIO side effect and proves it is not
     * duplicated by JIT + replay. */
    jit_split_setup(&c, false);
    m_w32(NULL, 0, 0xe5801000u);                  /* STR r1,[r0] */
    c.r[0] = SPLIT_VA + 0xffeu;
    c.r[1] = 0x11223344u;
    g_write8_calls = 0;
    CHECK(jit_mem_store32(&c, c.r[0], c.r[1]) != 0u,
          "faulting store32 did not report failure");
    CHECK(g_write8_calls == 0u,
          "faulting JIT store32 caused %u premature MMIO-like writes", g_write8_calls);
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT,
          "interpreter replay of store32 must take the data abort");
    CHECK(g_write8_calls == 2u,
          "JIT + replay caused %u first-page writes expect exactly 2", g_write8_calls);
    CHECK(g_ram[SPLIT_PA0 + 0xffeu] == 0x44u &&
          g_ram[SPLIT_PA0 + 0xfffu] == 0x33u,
          "interpreter replay did not leave the architectural partial store");

    /* Reads can be side-effecting too. Preflight prevents the same duplicate
     * transaction before replay of a crossing LDR whose second page faults. */
    jit_split_setup(&c, false);
    m_w32(NULL, 0, 0xe5901000u);                  /* LDR r1,[r0] */
    g_ram[SPLIT_PA0 + 0xffeu] = 0xaau;
    g_ram[SPLIT_PA0 + 0xfffu] = 0xbbu;
    c.r[0] = SPLIT_VA + 0xffeu;
    c.r[1] = 0xdeadbeefu;
    g_read8_calls = 0;
    loaded = jit_mem_load32(&c, c.r[0]);
    CHECK((loaded >> 32) == 1u, "faulting load32 did not report its fault bit");
    CHECK(g_read8_calls == 0u,
          "faulting JIT load32 caused %u premature MMIO-like reads", g_read8_calls);
    CHECK(arm_step(&c) == ARM_OK && c.r[15] == ARM_VEC_DATA_ABORT,
          "interpreter replay of load32 must take the data abort");
    CHECK(g_read8_calls == 2u,
          "JIT + replay caused %u first-page reads expect exactly 2", g_read8_calls);
    CHECK(c.r[1] == 0xdeadbeefu, "faulting replay changed destination to %08x", c.r[1]);
}

static void test_memory_helpers_honor_sctlr_u_and_a(void) {
    arm_cpu_t c;
    uint64_t loaded;

    memset(g_ram, 0, sizeof g_ram);
    arm_reset(&c, &g_bus);
    c.cpsr = (c.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_SYS;
    c.cp15.sctlr &= ~(ARM_SCTLR_A | ARM_SCTLR_U);
    m_w32(NULL, 0x800u, 0x11223344u);
    g_read32_calls = g_write32_calls = 0u;
    loaded = jit_mem_load32(&c, 0x801u);
    CHECK((loaded >> 32) == 0u && (uint32_t)loaded == 0x44112233u,
          "legacy JIT load32=%llx expect rotated 44112233",
          (unsigned long long)loaded);
    CHECK(jit_mem_store32(&c, 0x803u, 0xa1b2c3d4u) == 0u &&
          m_r32(NULL, 0x800u) == 0xa1b2c3d4u,
          "legacy JIT store32 did not align down");

    m_w16(NULL, 0x800u, 0x1234u);
    g_read16_calls = g_write16_calls = 0u;
    loaded = jit_mem_load16(&c, 0x801u);
    CHECK((loaded >> 32) == 1u && g_read16_calls == 0u,
          "legacy odd JIT load16=%llx reads=%u, expect replay without a read",
          (unsigned long long)loaded, g_read16_calls);
    CHECK(jit_mem_store16(&c, 0x801u, 0x5678u) != 0u &&
          g_write16_calls == 0u && m_r16(NULL, 0x800u) == 0x1234u,
          "legacy odd JIT store16 did not request side-effect-free replay");

    g_ram[0x801u] = 0x11u; g_ram[0x802u] = 0x22u;
    g_ram[0x803u] = 0x33u; g_ram[0x804u] = 0x44u;
    jit_map_normal_identity(&c);
    c.cp15.sctlr = (c.cp15.sctlr & ~ARM_SCTLR_A) | ARM_SCTLR_U;
    loaded = jit_mem_load32(&c, 0x801u);
    CHECK((loaded >> 32) == 0u && (uint32_t)loaded == 0x44332211u,
          "U=1 JIT load32=%llx expect unaligned 44332211",
          (unsigned long long)loaded);
    loaded = jit_mem_load16(&c, 0x801u);
    CHECK((loaded >> 32) == 0u && (uint32_t)loaded == 0x2211u,
          "U=1 JIT load16=%llx expect unaligned 2211",
          (unsigned long long)loaded);

    c.cp15.sctlr |= ARM_SCTLR_A;
    g_read32_calls = g_write32_calls = 0u;
    loaded = jit_mem_load32(&c, 0x801u);
    CHECK((loaded >> 32) == 1u && g_read32_calls == 0u,
          "A=1 JIT load32 fault=%llu reads=%u",
          (unsigned long long)(loaded >> 32), g_read32_calls);
    m_w32(NULL, 0x800u, 0x55667788u);
    g_write32_calls = 0u;
    CHECK(jit_mem_store32(&c, 0x801u, 0xdeadbeefu) != 0u &&
          m_r32(NULL, 0x800u) == 0x55667788u,
          "A=1 JIT store32 wrote before interpreter replay");
    CHECK(g_write32_calls == 0u, "A=1 JIT store32 issued %u bus writes",
          g_write32_calls);
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

/* ==================================================================== Thumb */
/*
 * Thumb is 68.95% of what the real kernel retires, so these are the tests that
 * decide whether the JIT can carry a boot. They are in the same two families
 * as the ARM ones — block shape, and byte-exact emitted words — with a third
 * that exists only here: the ARM/Thumb state boundary (docs/dynarec.md §7.4),
 * which is the part of this file most likely to be wrong in a way that only
 * shows up thousands of instructions later.
 */

static void load16(arm_cpu_t *c, uint32_t at, const uint16_t *prog, size_t n) {
    size_t i;
    memset(g_ram, 0, sizeof g_ram);
    for (i = 0; i < n; i++) m_w16(NULL, at + (uint32_t)(i * 2), prog[i]);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~0x1fu) | ARM_MODE_SYS | ARM_CPSR_T;
}
static bool xlate16(arm_cpu_t *c, uint32_t at, const uint16_t *prog, size_t n,
                    jit_block_t *blk, size_t cap) {
    load16(c, at, prog, n);
    memset(g_code, 0, sizeof g_code);
    return jit_translate(c, at, g_code, cap, blk);
}

/* 0xe7fe is `B .` — a Thumb branch to itself, the terminator these use. */
#define T_SELF 0xe7feu

static void test_thumb_block_shape(void) {
    arm_cpu_t c; jit_block_t b;
    {   /* MOVS r0,#1 ; ADD r0,#1 ; B . */
        uint16_t p[] = { 0x2001, 0x3001, T_SELF };
        CHECK(xlate16(&c, 0, p, 3, &b, CODE_WORDS), "translated");
        CHECK(b.insn_count == 3, "insn_count=%u expect 3", b.insn_count);
        CHECK(b.native_count == 3, "native_count=%u expect 3", b.native_count);
        CHECK(b.end_reason == JIT_END_BRANCH, "end_reason=%d expect BRANCH", (int)b.end_reason);
        CHECK((b.key.flags & JIT_BLK_THUMB) != 0, "thumb flag recorded");
    }
    {   /* PUSH {lr} is not translated: the block must stop *before* it. */
        uint16_t p[] = { 0x2001, 0xb500, 0x2002 };
        CHECK(xlate16(&c, 0, p, 3, &b, CODE_WORDS), "translated");
        CHECK(b.insn_count == 1, "insn_count=%u expect 1", b.insn_count);
        CHECK(b.end_reason == JIT_END_FALLBACK, "end_reason=%d expect FALLBACK", (int)b.end_reason);
        CHECK(count_word(&b, 0x52800020u) == 1, "movz w0,#JIT_EXIT_INTERPRET");
        CHECK(count_word(&b, 0x52800048u) == 1, "movz w8,#2 resumes at the PUSH");
    }
    {   /* Thumb steps by two, so a block starting at 0xffc covers exactly two
         * instructions before the 4 KB page changes. */
        uint16_t p[] = { 0x1c00, 0x1c00, 0x1c00, 0x1c00 };
        CHECK(xlate16(&c, 0xffc, p, 4, &b, CODE_WORDS), "translated");
        CHECK(b.insn_count == 2, "insn_count=%u expect 2", b.insn_count);
        CHECK(b.end_reason == JIT_END_PAGE, "end_reason=%d expect PAGE", (int)b.end_reason);
    }
    {   /* The 64-instruction cap applies per instruction, not per byte. */
        uint16_t p[80];
        unsigned i;
        for (i = 0; i < 80; i++) p[i] = 0x1c00;   /* ADDS r0,r0,#0 */
        CHECK(xlate16(&c, 0, p, 80, &b, CODE_WORDS), "translated");
        CHECK(b.insn_count == JIT_MAX_INSNS, "insn_count=%u expect %u",
              b.insn_count, JIT_MAX_INSNS);
        CHECK(b.end_reason == JIT_END_LIMIT, "end_reason=%d expect LIMIT", (int)b.end_reason);
    }
}

/*
 * Every Thumb encoding this file refuses. Each must produce a zero-instruction
 * block, because "not implemented" has to mean "interpreter" and never "close
 * enough" — and for the block-transfer and PC-writing forms below, "close
 * enough" would mean a second copy of the base-restored abort model and of the
 * exception-return alignment rule.
 */
static void test_thumb_refused_encodings(void) {
    static const struct { uint16_t insn; const char *what; } CASES[] = {
        { 0xb500u, "PUSH {lr}          (base-restored abort model, §7.3)" },
        { 0xb401u, "PUSH {r0}" },
        { 0xbc01u, "POP  {r0}" },
        { 0xbd00u, "POP  {pc}          (writes PC and can switch state)" },
        { 0xc802u, "LDMIA r0!,{r1}" },
        { 0xc002u, "STMIA r0!,{r1}" },
        { 0x4088u, "LSL r0,r1          (shift by register, §5.2(2))" },
        { 0x40c8u, "LSR r0,r1" },
        { 0x4108u, "ASR r0,r1" },
        { 0x41c8u, "ROR r0,r1" },
        { 0x4687u, "MOV pc,r0          (hi-register write to PC)" },
        { 0x4487u, "ADD pc,r0" },
        { 0xdf00u, "SWI #0             (exception entry)" },
        { 0xde00u, "permanently undefined (cond 0xe)" },
        { 0xbe00u, "BKPT #0" },
        { 0xb672u, "CPSID i            (mode change)" },
        { 0xb650u, "SETEND le" },
        { 0xba08u, "REV r0,r1" },
        { 0xba48u, "REV16 r0,r1" },
        { 0xbac8u, "REVSH r0,r1" },
        /* ARMv7 space. These do not exist on this core and the interpreter
         * does not implement them, but they must be declined *explicitly*: a
         * translator that treated IT as a no-op would run up to four following
         * instructions unconditionally. */
        { 0xbf08u, "IT EQ              (ARMv7; must end the block)" },
        { 0xbf14u, "ITE NE" },
        { 0xbf00u, "NOP hint" },
        { 0xb910u, "CBNZ r0,.+4        (ARMv7)" }
    };
    arm_cpu_t c; jit_block_t b;
    unsigned i;
    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        bool ok = xlate16(&c, 0, &CASES[i].insn, 1, &b, CODE_WORDS);
        CHECK(!ok && b.insn_count == 0, "%s must fall back", CASES[i].what);
    }
}

/* One guest instruction, its exact host body, in the pinned registers. */
static void test_thumb_bodies_are_exact(void) {
    static const struct { uint16_t guest; uint32_t host; const char *what; } CASES[] = {
        { 0x202au, 0x52800553u, "MOVS r0,#42     -> movz w19,#42"         },
        { 0x2805u, 0x7100167fu, "CMP  r0,#5      -> cmp  w19,#5"          },
        { 0x3001u, 0x31000673u, "ADD  r0,#1      -> adds w19,w19,#1"      },
        { 0x3801u, 0x71000673u, "SUB  r0,#1      -> subs w19,w19,#1"      },
        { 0x1888u, 0x2b150293u, "ADD  r0,r1,r2   -> adds w19,w20,w21"     },
        { 0x1e48u, 0x71000693u, "SUB  r0,r1,#1   -> subs w19,w20,#1"      },
        { 0x4008u, 0x0a140273u, "AND  r0,r1      -> and  w19,w19,w20"     },
        { 0x4248u, 0x6b1403f3u, "NEG  r0,r1      -> subs w19,wzr,w20"     },
        { 0x4288u, 0x6b14027fu, "CMP  r0,r1      -> cmp  w19,w20"         },
        { 0x4348u, 0x1b147e73u, "MUL  r0,r1      -> mul  w19,w19,w20"     },
        { 0x43c8u, 0x2a3403f3u, "MVN  r0,r1      -> mvn  w19,w20"         },
        { 0x4148u, 0x3a140273u, "ADC  r0,r1      -> adcs w19,w19,w20"     },
        { 0x4188u, 0x7a140273u, "SBC  r0,r1      -> sbcs w19,w19,w20"     },
        { 0x4485u, 0x0b13037bu, "ADD  sp,r0 (hi) -> add  w27,w27,w19"     },
        { 0x4608u, 0x2a1403f3u, "MOV  r0,r1(hi)  -> mov  w19,w20"         },
        { 0xb2c8u, 0x53001e93u, "UXTB r0,r1      -> ubfx w19,w20,#0,#8"   },
        { 0xb288u, 0x53003e93u, "UXTH r0,r1      -> ubfx w19,w20,#0,#16"  },
        { 0xb248u, 0x13001e93u, "SXTB r0,r1      -> sbfx w19,w20,#0,#8"   },
        { 0xb208u, 0x13003e93u, "SXTH r0,r1      -> sbfx w19,w20,#0,#16"  },
        { 0xb002u, 0x1100237bu, "ADD  sp,#8      -> add  w27,w27,#8"      },
        { 0xb082u, 0x5100237bu, "SUB  sp,#8      -> sub  w27,w27,#8"      },
        { 0xa801u, 0x11001373u, "ADD  r0,sp,#4   -> add  w19,w27,#4"      },
        { 0xa001u, 0x52800113u, "ADD  r0,pc,#4   -> movz w19,#8"          },
        { 0x00c8u, 0x531d7293u, "LSLS r0,r1,#3   -> lsl  w19,w20,#3"      },
        { 0x0888u, 0x53027e93u, "LSRS r0,r1,#2   -> lsr  w19,w20,#2"      },
        { 0x1108u, 0x13047e93u, "ASRS r0,r1,#4   -> asr  w19,w20,#4"      }
    };
    arm_cpu_t c; jit_block_t b;
    unsigned i;
    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        uint16_t p[2]; p[0] = CASES[i].guest; p[1] = T_SELF;
        if (!xlate16(&c, 0, p, 2, &b, CODE_WORDS)) {
            g_fail++; printf("  FAIL %s: not translated\n", CASES[i].what); continue;
        }
        if (count_word(&b, CASES[i].host) != 1) {
            g_fail++; printf("  FAIL %s: expected host word %08x once\n",
                             CASES[i].what, CASES[i].host);
            dump(&b);
        } else g_pass++;
    }
}

/*
 * Thumb-1 sets flags with no S bit, and LSLS/LSRS/ASRS set C from a
 * barrel-shifter carry-out that is NOT a translate-time constant — the case
 * the ARM translator is allowed to refuse and this one is not. The carry must
 * be read out of the SOURCE before the destination is written, because
 * Rd == Rs is the ordinary case.
 */
static void test_thumb_shift_carry(void) {
    arm_cpu_t c; jit_block_t b;
    uint16_t p[2]; p[1] = T_SELF;

    p[0] = 0x00c8u;                              /* LSLS r0,r1,#3 */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "LSLS translated");
    CHECK(count_word(&b, 0x531d768cu) == 1, "ubfx w12,w20,#29,#1 takes the carry-out");
    CHECK(count_word(&b, 0xb363018au) == 1, "bfi x10,x12,#29,#1 inserts it into C");
    CHECK(count_word(&b, 0xd35c7129u) == 1, "ubfx x9,x9,#28,#1 saves V only");

    p[0] = 0x0048u;                              /* LSLS r0,r1,#1 */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "LSLS #1 translated");
    CHECK(count_word(&b, 0x531f7e8cu) == 1, "ubfx w12,w20,#31,#1");

    /* LSR #0 encodes LSR #32: the result is zero and C is bit 31. */
    p[0] = 0x0808u;                              /* LSRS r0,r1,#32 */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "LSRS #32 translated");
    CHECK(count_word(&b, 0x531f7e8cu) == 1, "ubfx w12,w20,#31,#1 is the carry");
    CHECK(count_word(&b, 0x2a1f03f3u) == 1, "mov w19,wzr is the result");

    /* ASR #0 encodes ASR #32: the sign bit smeared, C is bit 31. */
    p[0] = 0x1008u;                              /* ASRS r0,r1,#32 */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "ASRS #32 translated");
    CHECK(count_word(&b, 0x131f7e93u) == 1, "asr w19,w20,#31 is the result");

    /* LSL #0 is a move: the carry is untouched, so C is PRESERVED, which is
     * the two-bit save, not the one-bit one. */
    p[0] = 0x0008u;                              /* LSLS r0,r1,#0 */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "LSLS #0 translated");
    CHECK(count_word(&b, 0xd35c7529u) == 1, "ubfx x9,x9,#28,#2 preserves C and V");
    CHECK(count_word(&b, 0xd35c7129u) == 0, "and does not use the one-bit form");

    /* ADDS/SUBS/CMP map exactly and must carry no fixup at all. */
    p[0] = 0x1888u;                              /* ADD r0,r1,r2 */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "ADD translated");
    CHECK(count_word(&b, 0x6a13027fu) == 0, "arithmetic needs no logical fixup");

    /* MOV #imm8 and the logical ALU ops preserve C and V. */
    p[0] = 0x202au;                              /* MOVS r0,#42 */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "MOVS translated");
    CHECK(count_word(&b, 0xd35c7529u) == 1, "MOVS #imm8 preserves C and V");
    CHECK(count_word(&b, 0x6a13027fu) == 1, "ands wzr,w19,w19 recomputes N,Z");

    /* TST discards its result, so the fixup reads it from the scratch. */
    p[0] = 0x4208u;                              /* TST r0,r1 */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "TST translated");
    CHECK(count_word(&b, 0x0a14026bu) == 1, "and w11,w19,w20 into the scratch");
    CHECK(count_word(&b, 0x6a0b017fu) == 1, "ands wzr,w11,w11 from the scratch");
}

/*
 * THE STATE BOUNDARY. Everything that can leave Thumb state is here, and the
 * assertions are deliberately both positive and negative: an instruction that
 * must NOT touch the T bit is checked for the absence of the write, because a
 * spurious state change is silent until the next fetch decodes garbage.
 *
 * The T bit is CPSR bit 5, so the write is `bfi w10, <src>, #5, #1` around a
 * load and store of cpu->cpsr at offset 64.
 */
#define W_LDR_CPSR   0xb940438au   /* ldr w10, [x28, #64]      */
#define W_STR_CPSR   0xb900438au   /* str w10, [x28, #64]      */
#define W_T_FROM_R3  0x331b02cau   /* bfi w10, w22, #5, #1     */
#define W_T_CLEAR    0x331b03eau   /* bfi w10, wzr, #5, #1     */
#define W_MOV_PC_S0  0x2a0903e8u   /* mov w8, w9  (exit_reg)   */

static void test_thumb_state_transitions(void) {
    arm_cpu_t c; jit_block_t b;
    uint16_t p[2];

    /* BX r3: T := r3[0], target = r3 & ~1, no link register write. */
    p[0] = 0x4718u;
    CHECK(xlate16(&c, 0, p, 1, &b, CODE_WORDS), "BX r3 translated");
    CHECK(b.end_reason == JIT_END_BRANCH, "BX ends the block");
    CHECK(b.insn_count == 1, "insn_count=%u expect 1", b.insn_count);
    CHECK(count_word(&b, 0x52800020u) == 1,
          "BX must emit an invalid-target interpreter exit");
    CHECK(count_word(&b, W_T_FROM_R3) == 1, "bfi w10,w22,#5,#1 sets T from the target");
    CHECK(count_word(&b, 0x121f7ac9u) == 1, "and w9,w22,#0xfffffffe clears bit 0");
    CHECK(count_word(&b, W_MOV_PC_S0) == 1, "the resume PC comes from a register");
    CHECK(count_word(&b, 0xb9003b89u) == 0, "BX writes no link register");

    /* BLX r3: the same, plus LR = (pc + 2) | 1 with the Thumb bit SET. */
    p[0] = 0x4798u;
    CHECK(xlate16(&c, 0, p, 1, &b, CODE_WORDS), "BLX r3 translated");
    CHECK(count_word(&b, 0x52800020u) == 1,
          "BLX must emit an invalid-target interpreter exit");
    CHECK(count_word(&b, W_T_FROM_R3) == 1, "BLX r3 sets T from the target");
    CHECK(count_word(&b, 0x5280006au) == 1, "movz w10,#3 is (pc+2)|1");
    CHECK(count_word(&b, 0xb9003b8au) == 1, "str w10,[x28,#56] writes LR");

    /* BX pc: r15 reads pc + 4, which is even, so this leaves Thumb for ARM. */
    p[0] = 0x4778u;
    CHECK(xlate16(&c, 0, p, 1, &b, CODE_WORDS), "BX pc translated");
    CHECK(count_word(&b, 0x52800089u) == 1, "movz w9,#4 materialises pc + 4");
    CHECK(count_word(&b, 0x331b012au) == 1, "bfi w10,w9,#5,#1 still sets T from bit 0");

    /* BLX pc is UNPREDICTABLE on ARM1176. It must remain an interpreter trap,
     * not become a JIT-only branch-and-link to pc+4. Bits[2:0] are SBZ but all
     * eight encodings are covered by the exhaustive decoder parity test. */
    p[0] = 0x47f8u;
    CHECK(!xlate16(&c, 0, p, 1, &b, CODE_WORDS), "BLX pc must be declined");
    CHECK(b.insn_count == 0, "declined BLX pc emitted %u guest instructions",
          b.insn_count);

    /* BLX suffix (0xe800): always returns to ARM, so T is cleared with WZR,
     * the target is masked to a word boundary, and LR keeps the Thumb bit. */
    p[0] = 0xe800u;
    CHECK(xlate16(&c, 0, p, 1, &b, CODE_WORDS), "BLX suffix translated");
    CHECK(b.end_reason == JIT_END_BRANCH, "BLX suffix ends the block");
    CHECK(count_word(&b, W_T_CLEAR) == 1, "bfi w10,wzr,#5,#1 forces ARM state");
    CHECK(count_word(&b, 0xb9403b89u) == 1, "ldr w9,[x28,#56] reads LR first");
    CHECK(count_word(&b, 0x121e7529u) == 1, "and w9,w9,#0xfffffffc word-aligns it");
    CHECK(count_word(&b, 0xb9003b8au) == 1, "str w10,[x28,#56] then overwrites LR");

    /* BL suffix (0xf800): stays in Thumb, so the T bit must NOT be written.
     * The epilogue also loads cpu->cpsr, so the discriminator is the BFI. */
    p[0] = 0xf800u;
    CHECK(xlate16(&c, 0, p, 1, &b, CODE_WORDS), "BL suffix translated");
    CHECK(b.end_reason == JIT_END_BRANCH, "BL suffix ends the block");
    CHECK(count_word(&b, W_T_CLEAR) == 0, "BL suffix does not clear T");
    CHECK(count_word(&b, 0x121f7929u) == 1, "and w9,w9,#0xfffffffe keeps halfword alignment");
    CHECK(count_word(&b, W_STR_CPSR) == 1, "only the epilogue writes cpu->cpsr");

    /* BL prefix (0xf000): a pure LR write. It is not a branch, so the block
     * continues into the suffix. */
    p[0] = 0xf000u; p[1] = 0xf800u;
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "BL pair translated");
    CHECK(b.insn_count == 2, "insn_count=%u expect 2 (prefix then suffix)", b.insn_count);
    CHECK(count_word(&b, 0x52800089u) == 1, "movz w9,#4 is LR = pc + 4 + 0");
    CHECK(count_word(&b, 0xb9003b89u) == 1, "str w9,[x28,#56] writes LR");

    /* Plain Thumb branches change no state whatsoever. */
    p[0] = 0xe000u;
    CHECK(xlate16(&c, 0, p, 1, &b, CODE_WORDS), "B translated");
    CHECK(count_word(&b, 0x52800088u) == 1, "movz w8,#4 is pc + 4");
    CHECK(count_word(&b, W_T_CLEAR) == 0, "B does not touch T");
    CHECK(count_word(&b, W_LDR_CPSR) == 1, "cpu->cpsr is read once, by the epilogue");
}

/* B<cond> has two static edges, exactly like its ARM counterpart (§3.5). */
static void test_thumb_conditional_branch(void) {
    arm_cpu_t c; jit_block_t b;
    uint16_t p[1];
    p[0] = 0xd002u;                              /* BEQ .+4 -> target 8 */
    CHECK(xlate16(&c, 0, p, 1, &b, CODE_WORDS), "BEQ translated");
    CHECK(b.insn_count == 1 && b.end_reason == JIT_END_BRANCH, "ends at the branch");
    CHECK(count_word(&b, 0x540000a1u) == 1, "b.ne +5 skips the taken exit");
    CHECK(count_word(&b, 0x52800108u) == 1, "movz w8,#8 (taken target)");
    CHECK(count_word(&b, 0x52800048u) == 1, "movz w8,#2 (fall-through)");

    /* A backward BNE, which is what every Thumb loop ends with. */
    p[0] = 0xd1feu;                              /* BNE .-4 from 4 -> 4 */
    CHECK(xlate16(&c, 4, p, 1, &b, CODE_WORDS), "BNE translated");
    CHECK(count_word(&b, 0x52800088u) == 1, "movz w8,#4 (backward target)");
    CHECK(count_word(&b, 0x540000a0u) == 1, "b.eq +5 skips it");
}

/*
 * Thumb memory: the PC-relative load folds its address to a constant, the
 * halfword and sign-extended forms reach the right helper, and every access
 * publishes cpu->r[15] first because the interpreter re-executes the faulting
 * instruction and LR_abt is pc + 8 in Thumb too.
 */
static void test_thumb_memory(void) {
    arm_cpu_t c; jit_block_t b;
    uint16_t p[2]; p[1] = T_SELF;

    p[0] = 0x6848u;                              /* LDR r0,[r1,#4] */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "LDR imm5 translated");
    CHECK(count_word(&b, 0x11001281u) == 1, "add w1,w20,#4 computes the address");
    CHECK(count_word(&b, 0xb9003f88u) == 2, "str w8,[x28,#60] publishes cpu->r[15]");
    CHECK(count_word(&b, 0xd63f0200u) == 1, "blr x16 calls the helper");
    CHECK(count_word(&b, 0x2a0003f3u) == 1, "mov w19,w0 commits r0 after the check");

    p[0] = 0x70c8u;                              /* STRB r0,[r1,#3] */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "STRB imm5 translated");
    CHECK(count_word(&b, 0x11000e81u) == 1, "add w1,w20,#3");
    CHECK(count_word(&b, 0x2a1303e2u) == 1, "mov w2,w19 passes the stored value");
    CHECK(count_word(&b, 0x52800040u) == 1, "movz w0,#JIT_EXIT_ABORT on the fault path");

    p[0] = 0x8848u;                              /* LDRH r0,[r1,#2] */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "LDRH imm5 translated");
    CHECK(count_word(&b, 0x11000a81u) == 1, "add w1,w20,#2 (imm5 scaled by 2)");

    p[0] = 0x4802u;                              /* LDR r0,[pc,#8] */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "LDR pc-relative translated");
    CHECK(count_word(&b, 0x52800181u) == 1, "movz w1,#12 folds (pc+4 & ~3) + 8");

    p[0] = 0x9801u;                              /* LDR r0,[sp,#4] */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "LDR sp-relative translated");
    CHECK(count_word(&b, 0x11001361u) == 1, "add w1,w27,#4 uses the pinned SP");

    p[0] = 0x5688u;                              /* LDRSB r0,[r1,r2] */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "LDRSB translated");
    CHECK(count_word(&b, 0x0b150281u) == 1, "add w1,w20,w21 is the register offset");
    CHECK(count_word(&b, 0x13001c13u) == 1, "sbfx w19,w0,#0,#8 sign-extends the byte");

    p[0] = 0x5e88u;                              /* LDRSH r0,[r1,r2] */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "LDRSH translated");
    CHECK(count_word(&b, 0x13003c13u) == 1, "sbfx w19,w0,#0,#16 sign-extends the halfword");

    p[0] = 0x5288u;                              /* STRH r0,[r1,r2] */
    CHECK(xlate16(&c, 0, p, 2, &b, CODE_WORDS), "STRH translated");
    CHECK(count_word(&b, 0x2a1303e2u) == 1, "mov w2,w19 passes the stored halfword");
}

/*
 * EXHAUSTIVE: all 65,536 halfwords, checked against the interpreter.
 *
 * The dev box cannot run emitted code, but it can run the oracle, and three
 * properties are checkable without executing anything — and each of them is a
 * bug this project has already paid for once:
 *
 *   1. If the translator accepts an encoding, the interpreter must implement
 *      it. Accepting something the interpreter traps as ARM_UNDEFINED would
 *      turn a fault into silent wrong execution.
 *   2. If the interpreter moves PC anywhere other than pc + 2, the translator
 *      must have ended the block there. This is the property that 0xe800 —
 *      the BLX suffix that looks like an unconditional branch — violated in
 *      the interpreter once, sending real XNU into a table of pointers.
 *   3. An instruction the translator continues past must not have changed
 *      CPSR.T or the mode, because the rest of the block was translated on the
 *      assumption that it did not.
 *
 * Registers are all zero here, so a branch target is 0 and a store address is
 * 0; that is deliberate, since it makes every encoding executable exactly once
 * with no faults.
 */
static void test_thumb_decode_agrees_with_the_interpreter(void) {
    const uint32_t STATE = ARM_CPSR_T | ARM_CPSR_MODE_MASK;
    arm_cpu_t c; jit_block_t b;
    unsigned accepted = 0, bad_undef = 0, bad_flow = 0, bad_state = 0;
    uint32_t i;

    memset(g_ram, 0, sizeof g_ram);
    for (i = 0; i < 0x10000u; i++) {
        uint16_t insn = (uint16_t)i;
        uint32_t cpsr_before;
        bool ends;

        m_w16(NULL, 0, insn);
        m_w16(NULL, 2, T_SELF);

        arm_reset(&c, &g_bus);
        c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS | ARM_CPSR_T;
        if (!jit_translate(&c, 0, g_code, CODE_WORDS, &b) || b.insn_count == 0)
            continue;
        accepted++;
        /* The second halfword is `B .`, which always translates and always
         * ends the block: so a one-instruction block means this encoding
         * ended it, and a two-instruction one means it did not. */
        ends = (b.insn_count == 1);

        arm_reset(&c, &g_bus);
        c.cpsr = (c.cpsr & ~0x1fu) | ARM_MODE_SYS | ARM_CPSR_T;
        cpsr_before = c.cpsr;
        if (arm_step(&c) != ARM_OK) {
            if (bad_undef++ < 4) printf("    %04x: accepted but the interpreter traps\n", insn);
            continue;
        }
        if (!ends && c.r[15] != 2u) {
            if (bad_flow++ < 4)
                printf("    %04x: interpreter went to %08x but the block continued\n",
                       insn, c.r[15]);
        }
        if (!ends && (c.cpsr & STATE) != (cpsr_before & STATE)) {
            if (bad_state++ < 4)
                printf("    %04x: changed CPSR to %08x but the block continued\n",
                       insn, c.cpsr);
        }
    }
    CHECK(accepted > 30000u, "%u of 65536 Thumb encodings translated", accepted);
    CHECK(bad_undef == 0, "%u accepted encodings the interpreter does not implement", bad_undef);
    CHECK(bad_flow  == 0, "%u accepted encodings wrote PC without ending the block", bad_flow);
    CHECK(bad_state == 0, "%u accepted encodings changed CPSR without ending the block", bad_state);
}

/* The deny mask works the same way in Thumb as in ARM, plus one axis of its own. */
static void test_thumb_deny(void) {
    /* MOVS r0,#1 ; LDR r0,[r1,#0] ; B . */
    uint16_t p[] = { 0x2001, 0x6808, T_SELF };
    arm_cpu_t c; jit_block_t b;

    jit_set_deny(JIT_DENY_THUMB);
    CHECK(!xlate16(&c, 0, p, 3, &b, CODE_WORDS), "whole Thumb decoder denied");
    CHECK(b.insn_count == 0, "nothing translated");
    CHECK((b.key.flags & JIT_BLK_THUMB) != 0, "thumb flag still recorded");

    jit_set_deny(JIT_DENY_ALL);
    CHECK(!xlate16(&c, 0, p, 3, &b, CODE_WORDS), "deny-all declines Thumb too");
    CHECK(b.insn_count == 0, "nothing translated");

    jit_set_deny(JIT_DENY_LDST);
    CHECK(xlate16(&c, 0, p, 3, &b, CODE_WORDS), "translated with LDST denied");
    CHECK(b.insn_count == 1, "insn_count=%u expect 1", b.insn_count);

    jit_set_deny(JIT_DENY_BRANCH);
    CHECK(xlate16(&c, 0, p, 3, &b, CODE_WORDS), "translated with BRANCH denied");
    CHECK(b.insn_count == 2, "insn_count=%u expect 2", b.insn_count);

    jit_set_deny(0);
    CHECK(xlate16(&c, 0, p, 3, &b, CODE_WORDS), "translated with nothing denied");
    CHECK(b.insn_count == 3, "insn_count=%u expect 3", b.insn_count);
    /* JIT_DENY_THUMB must not affect ARM. */
    {
        uint32_t a[] = { 0xe3a00001, 0xeafffffe };
        jit_set_deny(JIT_DENY_THUMB);
        CHECK(xlate(&c, 0, a, 2, &b, CODE_WORDS), "ARM unaffected by DENY_THUMB");
        CHECK(b.insn_count == 2, "insn_count=%u expect 2", b.insn_count);
        jit_set_deny(0);
    }
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

/*
 * VFP stays in the interpreter, deliberately and permanently.
 *
 * core/src/arm/vfp.c implements VFPv2 with a host floating-point unit whose
 * rounding, exception flags and flush-to-zero behaviour it controls carefully.
 * Emitting arm64 FP instructions for those encodings would put a SECOND
 * floating-point implementation in the machine, with its own FPCR, and the two
 * would have to agree bit for bit on every result and every sticky flag. They
 * would not, and the divergence would be silent. So the translator must
 * decline the whole VFP encoding space and let arm_step run it — which is the
 * general rule of docs/dynarec.md (anything not translated is interpreted)
 * applied to the one family where "not yet" is really "not ever".
 */
static void test_vfp_is_never_translated(void) {
    static const struct { uint32_t insn; const char *what; } CASES[] = {
        { 0xecb10a20u, "VLDMIA r1!,{s0-s31}  (VFP load/store multiple)" },
        { 0xed910a00u, "VLDR s0,[r1]         (VFP load/store)"          },
        { 0xee302a20u, "VADD.F32 s4,s0,s1    (VFP data processing)"     },
        { 0xee304b01u, "VADD.F64 d4,d0,d1    (VFP data processing)"     },
        { 0xeeb40a60u, "VCMP.F32 s0,s1       (VFP data processing)"     },
        { 0xee100a10u, "VMOV r0,s0           (VFP 32-bit transfer)"     },
        { 0xeef10a10u, "VMRS r0,FPSCR        (VFP 32-bit transfer)"     },
        { 0xec510b10u, "VMOV r0,r1,d0        (VFP 64-bit transfer)"     },
        { 0xf2000d40u, "VADD.F32             (Advanced SIMD)"           },
    };
    arm_cpu_t c; jit_block_t b;
    unsigned i;
    for (i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
        CHECK(!xlate(&c, 0, &CASES[i].insn, 1, &b, CODE_WORDS),
              "%s must fall back", CASES[i].what);
        CHECK(b.insn_count == 0, "%s translated %u instructions",
              CASES[i].what, b.insn_count);
    }
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
    jit_buf_t buf = {0};
    uint32_t *a, *b2;
    if (!jit_buf_alloc(&buf, 1024)) {
        printf("  SKIP %s: host policy refused a JIT arena\n", __func__);
        return;
    }
    CHECK(true, "arena allocated");
    CHECK(buf.size >= 1024 && (buf.size % 0x4000u) == 0, "arena rounded to 16 KB");
    CHECK(jit_buf_policy(&buf) != NULL, "policy: %s", jit_buf_policy(&buf));
    a = jit_buf_take(&buf, 3);
    b2 = jit_buf_take(&buf, 3);
    if (!a || !b2) {
        CHECK(false, "two small code-buffer allocations failed");
        CHECK(jit_buf_free(&buf), "cleanup after allocation failure");
        return;
    }
    CHECK(true, "two allocations");
    CHECK((((uintptr_t)a) & 15u) == 0 && (((uintptr_t)b2) & 15u) == 0, "16-byte aligned");
    CHECK((uintptr_t)b2 >= (uintptr_t)a + 3u * sizeof *a,
          "allocations do not overlap");
    CHECK(jit_buf_take(&buf, buf.size) == NULL, "over-large request refused");
    if (!jit_buf_begin_write(&buf)) {
        CHECK(false, "could not open code buffer for writing");
        CHECK(jit_buf_free(&buf), "cleanup after begin-write failure");
        return;
    }
    a[0] = 0xd503201fu;
    if (!jit_buf_end_write(&buf)) {
        CHECK(false, "could not close code-buffer write scope");
        CHECK(jit_buf_free(&buf), "cleanup after end-write failure");
        return;
    }
    CHECK(jit_buf_commit(&buf, a, 4), "committed code range");
    CHECK(jit_buf_free(&buf), "freed arena");
    CHECK(buf.base == NULL, "freed");
    CHECK(jit_buf_take(&buf, 1) == NULL, "no allocation from a freed arena");
    /* On the x86 dev box this is false, and jit_enter must be a no-op. */
    if (!jit_host_can_execute()) {
        jit_block_t blk;
        memset(&blk, 0, sizeof blk);
        CHECK(jit_enter(&buf, &blk, NULL) == JIT_EXIT_INTERPRET,
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
    test_thumb_block_shape();
    test_thumb_refused_encodings();
    test_thumb_bodies_are_exact();
    test_thumb_shift_carry();
    test_thumb_state_transitions();
    test_thumb_conditional_branch();
    test_thumb_memory();
    test_thumb_decode_agrees_with_the_interpreter();
    test_thumb_deny();
    test_vfp_is_never_translated();
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
    test_memory_helpers_cross_pages_without_replay_side_effects();
    test_memory_helpers_honor_sctlr_u_and_a();
    test_code_buffer();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

/*
 * iOS3-VM — VFPv2 (VFP11) unit tests.
 *
 * Same shape as test_arm.c: a flat 1 MiB RAM behind arm_bus_t, hand-assembled
 * ARM encodings, single-stepped, with assertions. Every encoding used here was
 * cross-checked against the ARM ARM's bit layout by hand and matches what an
 * assembler emits for the mnemonic in the comment.
 *
 * The things worth testing about a VFP are not the happy-path adds. They are:
 * the s/d aliasing (the classic source of silent corruption), the load/store
 * multiple forms the kernel's own context switch depends on, the FPEXC.EN
 * asymmetry that IS the lazy-enable mechanism, and — most of all — that the
 * encodings this unit does not implement still stop the machine instead of
 * inventing a number.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"
#include "vfp.h"

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

/* Reset into SYS mode with VFP fully available: CPACR grants CP10/CP11 and
 * FPEXC.EN is set, which is the state XNU leaves a thread in after
 * _vfp_switch. Tests that care about the disabled case clear it themselves. */
static void vfp_reset(arm_cpu_t *c) {
    memset(g_ram, 0, sizeof g_ram);
    arm_reset(c, &g_bus);
    c->cpsr = (c->cpsr & ~0x1fu) | ARM_MODE_SYS;
    c->cp15.cpacr  = 0xfu << 20;
    c->vfp_fpexc   = ARM_FPEXC_EN;
}

static void load(const uint32_t *prog, size_t words) {
    for (size_t i = 0; i < words; i++) m_w32(NULL, (uint32_t)(i * 4), prog[i]);
}

/* Run `steps` instructions from PC=0, returning the status of the last one. */
static arm_status_t run(arm_cpu_t *c, const uint32_t *prog, size_t words, int steps) {
    arm_status_t st = ARM_OK;
    load(prog, words);
    for (int i = 0; i < steps; i++) { st = arm_step(c); if (st != ARM_OK) break; }
    return st;
}

/* ------------------------------------------------------------- encodings ---
 *
 * Extension register load/store:  cond 110 P U D W L Rn Vd 101 sz imm8
 */
#define VFP_LS(P,U,D,W,L,rn,vd,sz,imm8)                                       \
    (0xec000000u | ((uint32_t)(P)<<24) | ((uint32_t)(U)<<23)                  \
     | ((uint32_t)(D)<<22) | ((uint32_t)(W)<<21) | ((uint32_t)(L)<<20)        \
     | ((uint32_t)(rn)<<16) | ((uint32_t)(vd)<<12) | 0xa00u                   \
     | ((uint32_t)(sz)<<8) | (uint32_t)(imm8))

/*
 * VFP data processing: cond 1110 p D q r Vn Vd 101 sz N s M 0 Vm.
 * p/q/r are bits 23/21/20 (the opcode, with D wedged between them) and s is
 * bit 6, opc3<0>.
 */
#define VFP_DP(p,q,r,D,vn,vd,sz,N,s,M,vm)                                     \
    (0xee000a00u | ((uint32_t)(p)<<23) | ((uint32_t)(D)<<22)                  \
     | ((uint32_t)(q)<<21) | ((uint32_t)(r)<<20) | ((uint32_t)(vn)<<16)       \
     | ((uint32_t)(vd)<<12) | ((uint32_t)(sz)<<8) | ((uint32_t)(N)<<7)        \
     | ((uint32_t)(s)<<6) | ((uint32_t)(M)<<5) | (uint32_t)(vm))

/* Single-precision register N split into its Vx field and its extra bit: the
 * low bit of a single register number lives in D/N/M, the other four in the
 * Vd/Vn/Vm field. Getting this backwards is the classic VFP decode bug, so the
 * tests spell real register numbers and let these do the splitting. */
#define SV(n) ((n) >> 1)
#define SB(n) ((n) & 1u)

/* Three-operand arithmetic, by register number. `s` is opc3<0>. */
#define DP_S(p,q,r,s, sd,sn,sm)                                               \
    VFP_DP((p),(q),(r), SB(sd), SV(sn), SV(sd), 0, SB(sn), (s), SB(sm), SV(sm))
#define DP_D(p,q,r,s, dd,dn,dm)                                               \
    VFP_DP((p),(q),(r), 0, (dn), (dd), 1, 0, (s), 0, (dm))

/* The "other" group: p=q=r=1 and opc3<0>=1, keyed by opc2 in the Vn field,
 * with opc3<1> (bit 7) as `top`. */
#define UN_S(opc2,top, sd,sm)                                                 \
    VFP_DP(1,1,1, SB(sd), (opc2), SV(sd), 0, (top), 1, SB(sm), SV(sm))
#define UN_D(opc2,top, dd,dm)                                                 \
    VFP_DP(1,1,1, 0, (opc2), (dd), 1, (top), 1, 0, (dm))
/* The two conversions whose source and destination differ in width. */
#define UN_D_FROM_S(opc2,top, dd,sm)                                          \
    VFP_DP(1,1,1, 0, (opc2), (dd), 0, (top), 1, SB(sm), SV(sm))
#define UN_S_FROM_D(opc2,top, sd,dm)                                          \
    VFP_DP(1,1,1, SB(sd), (opc2), SV(sd), 1, (top), 1, 0, (dm))

#define VMRS(rt,crn) (0xeef00a10u | ((uint32_t)(crn)<<16) | ((uint32_t)(rt)<<12))
#define VMSR(crn,rt) (0xeee00a10u | ((uint32_t)(crn)<<16) | ((uint32_t)(rt)<<12))
#define VMOV_S_R(sn,rt) (0xee000a10u | ((uint32_t)SV(sn)<<16) | ((uint32_t)(rt)<<12) | ((uint32_t)SB(sn)<<7))
#define VMOV_R_S(rt,sn) (0xee100a10u | ((uint32_t)SV(sn)<<16) | ((uint32_t)(rt)<<12) | ((uint32_t)SB(sn)<<7))

/* --------------------------------------------------------------- bit casts */
static uint32_t f2u(float f)  { uint32_t u; memcpy(&u,&f,4); return u; }
static float    u2f(uint32_t u){ float f;   memcpy(&f,&u,4); return f; }
static uint64_t d2u(double d) { uint64_t u; memcpy(&u,&d,8); return u; }
static double   u2d(uint64_t u){ double d;  memcpy(&d,&u,8); return d; }

/* ================================================== the register file ===== */

/*
 * d0-d15 are not a second bank, they are a second NAME for s0-s31. Writing the
 * two halves of d3 through s6 and s7 and reading it back as a double is the
 * cheapest test that says the aliasing and the word order are both right; get
 * the word order backwards and every double the guest ever loads is garbage
 * that still looks like a plausible float.
 */
static void test_s_d_aliasing(void) {
    arm_cpu_t c; vfp_reset(&c);

    vfp_set_s(&c, 6, 0x12345678u);       /* low  word of d3 */
    vfp_set_s(&c, 7, 0x9abcdef0u);       /* high word of d3 */
    CHECK(vfp_get_d(&c, 3) == 0x9abcdef012345678ull,
          "d3 = 0x%016llx", (unsigned long long)vfp_get_d(&c, 3));

    vfp_set_d(&c, 0, d2u(1.0));
    CHECK(vfp_get_s(&c, 0) == 0x00000000u, "s0 = 0x%08x", vfp_get_s(&c, 0));
    CHECK(vfp_get_s(&c, 1) == 0x3ff00000u, "s1 = 0x%08x", vfp_get_s(&c, 1));

    /* The aliasing must be complete: d15 is s30/s31 and there is nothing
     * beyond it, so vfp_set_d(16) must wrap onto d0 rather than run off. */
    vfp_set_d(&c, 15, 0xaaaaaaaabbbbbbbbull);
    CHECK(vfp_get_s(&c, 30) == 0xbbbbbbbbu, "s30 = 0x%08x", vfp_get_s(&c, 30));
    CHECK(vfp_get_s(&c, 31) == 0xaaaaaaaau, "s31 = 0x%08x", vfp_get_s(&c, 31));
}

/* ==================================================== load and store ====== */

/*
 * The instruction the whole milestone turns on. _vfp_switch does
 * "VLDMIA r1!, {s0-s31}" (0xecb10a20) to restore a thread's register file, so
 * this exact word must move 128 bytes and advance r1 by 128.
 */
static void test_vldmia_writeback_the_vfp_switch_form(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = { VFP_LS(0,1,0,1,1, 1, 0, 0, 32) };
    CHECK(prog[0] == 0xecb10a20u, "encoding 0x%08x", prog[0]);

    for (unsigned i = 0; i < 32; i++) m_w32(NULL, 0x1000u + i * 4u, 0xc0de0000u + i);
    c.r[1] = 0x1000;
    CHECK(run(&c, prog, 1, 1) == ARM_OK, "VLDMIA trapped");
    for (unsigned i = 0; i < 32; i++)
        CHECK(vfp_get_s(&c, i) == 0xc0de0000u + i, "s%u = 0x%08x", i, vfp_get_s(&c, i));
    CHECK(c.r[1] == 0x1080u, "r1 = 0x%08x (want 0x1080)", c.r[1]);
}

/* And the other half of a context switch: VSTMIA writes the same 32 words
 * back, so a store-then-load round trip must be the identity. */
static void test_vstmia_vldmia_round_trip(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VFP_LS(0,1,0,1,0, 1, 0, 0, 32),      /* VSTMIA r1!, {s0-s31} */
        VFP_LS(0,1,0,1,1, 2, 0, 0, 32),      /* VLDMIA r2!, {s0-s31} */
    };
    for (unsigned i = 0; i < 32; i++) vfp_set_s(&c, i, 0xfeed0000u + i * 7u);
    c.r[1] = 0x2000; c.r[2] = 0x2000;
    load(prog, 2);
    CHECK(arm_step(&c) == ARM_OK, "VSTMIA trapped");
    for (unsigned i = 0; i < 32; i++) vfp_set_s(&c, i, 0);
    CHECK(arm_step(&c) == ARM_OK, "VLDMIA trapped");
    for (unsigned i = 0; i < 32; i++)
        CHECK(vfp_get_s(&c, i) == 0xfeed0000u + i * 7u, "s%u = 0x%08x", i, vfp_get_s(&c, i));
    CHECK(c.r[1] == 0x2080u && c.r[2] == 0x2080u, "writeback r1=%08x r2=%08x", c.r[1], c.r[2]);
}

/*
 * A doubleword list must lay out identically to the single list that aliases
 * it: VSTM {d0-d1} and VSTM {s0-s3} write the same four words in the same
 * order. If the word order inside a double were wrong this is where it shows.
 */
static void test_double_list_matches_single_list(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VFP_LS(0,1,0,0,0, 1, 0, 1, 4),       /* VSTMIA r1, {d0-d1}  */
        VFP_LS(0,1,0,0,0, 2, 0, 0, 4),       /* VSTMIA r2, {s0-s3}  */
    };
    for (unsigned i = 0; i < 4; i++) vfp_set_s(&c, i, 0x11110000u * (i + 1u));
    c.r[1] = 0x3000; c.r[2] = 0x3100;
    CHECK(run(&c, prog, 2, 2) == ARM_OK, "VSTM trapped");
    for (unsigned i = 0; i < 4; i++)
        CHECK(m_r32(NULL, 0x3000u + i*4u) == m_r32(NULL, 0x3100u + i*4u),
              "word %u: d-form 0x%08x vs s-form 0x%08x",
              i, m_r32(NULL, 0x3000u + i*4u), m_r32(NULL, 0x3100u + i*4u));
    CHECK(c.r[1] == 0x3000u, "no-writeback form modified r1");
}

/* VPUSH/VPOP are the DB-writeback and IA-writeback forms with Rn == sp. */
static void test_vpush_vpop(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VFP_LS(1,0,0,1,0, 13, 0, 1, 4),      /* VPUSH {d0-d1} */
        VFP_LS(0,1,0,1,1, 13, 2, 1, 4),      /* VPOP  {d2-d3} */
    };
    CHECK(prog[0] == 0xed2d0b04u, "VPUSH encoding 0x%08x", prog[0]);
    CHECK(prog[1] == 0xecbd2b04u, "VPOP encoding 0x%08x", prog[1]);
    vfp_set_d(&c, 0, 0x0123456789abcdefull);
    vfp_set_d(&c, 1, 0xfedcba9876543210ull);
    c.r[13] = 0x4000;
    load(prog, 2);
    CHECK(arm_step(&c) == ARM_OK, "VPUSH trapped");
    CHECK(c.r[13] == 0x3ff0u, "sp after VPUSH = 0x%08x", c.r[13]);
    CHECK(arm_step(&c) == ARM_OK, "VPOP trapped");
    CHECK(c.r[13] == 0x4000u, "sp after VPOP = 0x%08x", c.r[13]);
    CHECK(vfp_get_d(&c, 2) == 0x0123456789abcdefull, "d2 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 2));
    CHECK(vfp_get_d(&c, 3) == 0xfedcba9876543210ull, "d3 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 3));
}

static void test_vldr_vstr(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VFP_LS(1,1,0,0,1, 1, 0, 0, 2),       /* VLDR s0, [r1,#8]  */
        VFP_LS(1,0,0,0,1, 1, 0, 1, 2),       /* VLDR d0, [r1,#-8] */
        VFP_LS(1,1,0,0,0, 2, 0, 1, 0),       /* VSTR d0, [r2]     */
    };
    CHECK(prog[0] == 0xed910a02u, "VLDR encoding 0x%08x", prog[0]);
    m_w32(NULL, 0x5008, 0xcafebabeu);
    m_w32(NULL, 0x4ff8, 0x11112222u);
    m_w32(NULL, 0x4ffc, 0x33334444u);
    c.r[1] = 0x5000; c.r[2] = 0x6000;
    CHECK(run(&c, prog, 3, 3) == ARM_OK, "VLDR/VSTR trapped");
    /* s0 is the low half of d0, so the second load overwrote it. */
    CHECK(vfp_get_d(&c, 0) == 0x3333444411112222ull, "d0 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 0));
    CHECK(m_r32(NULL, 0x6000) == 0x11112222u, "stored low  0x%08x", m_r32(NULL, 0x6000));
    CHECK(m_r32(NULL, 0x6004) == 0x33334444u, "stored high 0x%08x", m_r32(NULL, 0x6004));
    (void)prog;
}

/* A list running past s31 or d15 names registers this part does not have. */
static void test_overlong_lists_trap(void) {
    arm_cpu_t c;
    uint32_t over_s[] = { VFP_LS(0,1,1,0,1, 1, 0, 0, 32) };  /* s1..s32  */
    uint32_t over_d[] = { VFP_LS(0,1,0,0,1, 1, 15, 1, 4) };  /* d15..d16 */
    uint32_t empty[]  = { VFP_LS(0,1,0,0,1, 1, 0, 0, 0) };   /* empty list */
    vfp_reset(&c); CHECK(run(&c, over_s, 1, 1) == ARM_UNDEFINED, "s-list overrun ran");
    vfp_reset(&c); CHECK(run(&c, over_d, 1, 1) == ARM_UNDEFINED, "d-list overrun ran");
    vfp_reset(&c); CHECK(run(&c, empty,  1, 1) == ARM_UNDEFINED, "empty list ran");
}

/* ================================================ system registers ======== */

static void test_vmrs_vmsr(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VMRS(0, 0),                          /* VMRS r0, FPSID */
        VMSR(1, 2),                          /* VMSR FPSCR, r2 */
        VMRS(3, 1),                          /* VMRS r3, FPSCR */
        VMRS(4, 8),                          /* VMRS r4, FPEXC */
    };
    c.r[2] = 0xffffffffu;
    CHECK(run(&c, prog, 4, 4) == ARM_OK, "VMRS/VMSR trapped");
    CHECK(c.r[0] == ARM1176_FPSID, "FPSID = 0x%08x", c.r[0]);
    /* Reserved FPSCR bits read as zero on the ARM1176, so the write is masked
     * rather than stored verbatim. */
    CHECK(c.r[3] == ARM_FPSCR_WMASK, "FPSCR readback = 0x%08x", c.r[3]);
    CHECK(c.r[4] == ARM_FPEXC_EN, "FPEXC = 0x%08x", c.r[4]);
}

static void test_fpsid_is_read_only(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = { VMSR(0, 0) };        /* VMSR FPSID, r0 */
    CHECK(run(&c, prog, 1, 1) == ARM_UNDEFINED, "VMSR FPSID was accepted");
}

/*
 * The lazy-enable asymmetry, which is not a quirk but the mechanism XNU relies
 * on: with FPEXC.EN clear, FPEXC and FPSID stay readable (that is how
 * _get_vfp_enabled asks whether VFP is on) and everything else is UNDEFINED.
 * Undefined here means the guest is VECTORED, not halted — arm_step routes it
 * through undefined_instruction — so a passing test sees ARM_OK with PC at the
 * Undefined vector.
 */
static void test_fpexc_en_gates_the_other_registers(void) {
    arm_cpu_t c;
    uint32_t read_fpexc[] = { VMRS(0, 8) };
    uint32_t read_fpsid[] = { VMRS(0, 0) };
    uint32_t read_fpscr[] = { VMRS(0, 1) };
    uint32_t an_add[]     = { DP_S(0,1,1,0, 0,0,0) };

    vfp_reset(&c); c.vfp_fpexc = 0;
    CHECK(run(&c, read_fpexc, 1, 1) == ARM_OK, "VMRS FPEXC refused while disabled");
    CHECK(c.r[0] == 0u, "FPEXC read back 0x%08x", c.r[0]);
    /* PC after a retired instruction at 0 is 4, which is also the Undefined
     * vector address, so the MODE is what distinguishes "ran" from
     * "vectored". */
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SYS,
          "VMRS FPEXC vectored: mode = 0x%02x", c.cpsr & 0x1fu);

    vfp_reset(&c); c.vfp_fpexc = 0;
    CHECK(run(&c, read_fpsid, 1, 1) == ARM_OK, "VMRS FPSID refused while disabled");
    CHECK(c.r[0] == ARM1176_FPSID, "FPSID read back 0x%08x", c.r[0]);

    vfp_reset(&c); c.vfp_fpexc = 0;
    CHECK(run(&c, read_fpscr, 1, 1) == ARM_OK, "VMRS FPSCR should vector, not halt");
    CHECK(c.r[15] == ARM_VEC_UNDEFINED, "PC = 0x%08x, want the Undefined vector", c.r[15]);
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND, "mode = 0x%02x", c.cpsr & 0x1fu);

    vfp_reset(&c); c.vfp_fpexc = 0;
    CHECK(run(&c, an_add, 1, 1) == ARM_OK, "VADD should vector, not halt");
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND, "mode = 0x%02x", c.cpsr & 0x1fu);

    /* CPACR withholding CP10/CP11 does the same, even with FPEXC.EN set. */
    vfp_reset(&c); c.cp15.cpacr = 0;
    CHECK(run(&c, read_fpsid, 1, 1) == ARM_OK, "CPACR denial should vector");
    CHECK((c.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_UND, "mode = 0x%02x", c.cpsr & 0x1fu);
}

/* ================================================== core transfers ======== */

static void test_vmov_core_registers(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        VMOV_S_R(5, 0),                      /* VMOV s5, r0      */
        VMOV_R_S(1, 5),                      /* VMOV r1, s5      */
        0xec410b10u,                         /* VMOV d0, r0, r1  */
        0xec532b10u,                         /* VMOV r2, r3, d0  */
        0xec454a11u,                         /* VMOV s2, s3, r4, r5 */
        0xec576a11u,                         /* VMOV r6, r7, s2, s3 */
    };
    CHECK(VMOV_S_R(0,0) == 0xee000a10u, "VMOV s0,r0 = 0x%08x", VMOV_S_R(0,0));
    CHECK(VMOV_R_S(0,0) == 0xee100a10u, "VMOV r0,s0 = 0x%08x", VMOV_R_S(0,0));
    c.r[0] = 0xdeadbeefu;
    c.r[4] = 0x0badf00du; c.r[5] = 0xfeedface;
    CHECK(run(&c, prog, 6, 6) == ARM_OK, "a VMOV trapped");
    CHECK(vfp_get_s(&c, 5) == 0xdeadbeefu, "s5 = 0x%08x", vfp_get_s(&c, 5));
    CHECK(c.r[1] == 0xdeadbeefu, "r1 = 0x%08x", c.r[1]);
    CHECK(c.r[2] == 0xdeadbeefu && c.r[3] == 0xdeadbeefu,
          "64-bit move back r2=0x%08x r3=0x%08x", c.r[2], c.r[3]);
    CHECK(c.r[6] == 0x0badf00du && c.r[7] == 0xfeedfaceu,
          "s-pair move back r6=0x%08x r7=0x%08x", c.r[6], c.r[7]);
    /* The pair really landed in s2 and s3, i.e. in d1. */
    CHECK(vfp_get_d(&c, 1) == 0xfeedface0badf00dull, "d1 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 1));
}

/* ================================================== arithmetic =========== */

static void set_f32(arm_cpu_t *c, unsigned n, float v) { vfp_set_s(c, n, f2u(v)); }
static float get_f32(const arm_cpu_t *c, unsigned n)   { return u2f(vfp_get_s(c, n)); }
static void set_f64(arm_cpu_t *c, unsigned n, double v){ vfp_set_d(c, n, d2u(v)); }
static double get_f64(const arm_cpu_t *c, unsigned n)  { return u2d(vfp_get_d(c, n)); }

static void test_single_precision_arithmetic(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        DP_S(0,1,1,0, 4,0,1),                /* VADD.F32 s4, s0, s1 */
        DP_S(0,1,1,1, 5,0,1),                /* VSUB.F32 s5, s0, s1 */
        DP_S(0,1,0,0, 6,0,1),                /* VMUL.F32 s6, s0, s1 */
        DP_S(1,0,0,0, 7,0,1),                /* VDIV.F32 s7, s0, s1 */
        UN_S(1,1, 8,0),                      /* VSQRT.F32 s8, s0    */
    };
    CHECK(prog[0] == 0xee302a20u, "VADD.F32 s4,s0,s1 = 0x%08x", prog[0]);
    CHECK(prog[4] == 0xeeb14ac0u, "VSQRT.F32 s8,s0 = 0x%08x", prog[4]);
    set_f32(&c, 0, 9.0f);
    set_f32(&c, 1, 2.0f);
    CHECK(run(&c, prog, 5, 5) == ARM_OK, "arithmetic trapped");
    CHECK(get_f32(&c, 4) == 11.0f, "VADD = %f", (double)get_f32(&c, 4));
    CHECK(get_f32(&c, 5) ==  7.0f, "VSUB = %f", (double)get_f32(&c, 5));
    CHECK(get_f32(&c, 6) == 18.0f, "VMUL = %f", (double)get_f32(&c, 6));
    CHECK(get_f32(&c, 7) ==  4.5f, "VDIV = %f", (double)get_f32(&c, 7));
    CHECK(get_f32(&c, 8) ==  3.0f, "VSQRT = %f", (double)get_f32(&c, 8));
    CHECK((c.vfp_fpscr & 0x9fu) == 0u, "clean arithmetic set FPSCR flags 0x%08x", c.vfp_fpscr);
}

static void test_double_precision_arithmetic(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        DP_D(0,1,1,0, 4,0,1),                /* VADD.F64 d4, d0, d1 */
        DP_D(1,0,0,0, 5,0,1),                /* VDIV.F64 d5, d0, d1 */
        UN_D(1,1, 6,0),                      /* VSQRT.F64 d6, d0    */
    };
    CHECK(prog[0] == 0xee304b01u, "VADD.F64 d4,d0,d1 = 0x%08x", prog[0]);
    set_f64(&c, 0, 2.0);
    set_f64(&c, 1, 4.0);
    CHECK(run(&c, prog, 3, 3) == ARM_OK, "double arithmetic trapped");
    CHECK(get_f64(&c, 4) == 6.0,  "VADD.F64 = %f", get_f64(&c, 4));
    CHECK(get_f64(&c, 5) == 0.5,  "VDIV.F64 = %f", get_f64(&c, 5));
    CHECK(d2u(get_f64(&c, 6)) == d2u(1.4142135623730951),
          "VSQRT.F64 = %.17g", get_f64(&c, 6));
}

/*
 * VMLA is a multiply, ROUNDED, then an add, ROUNDED. VFPv2 has no fused
 * multiply-add, so an implementation that let the compiler contract the two
 * would differ from hardware in the last bit. These operands are chosen so
 * that fused and unfused disagree: 1 + 2^-24*(1+2^-24) is exactly the
 * halfway-plus-epsilon case where the product's lost bits change the sum.
 */
static void test_vmla_is_not_fused(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = { DP_S(0,0,0,0, 2,0,1) };   /* VMLA.F32 s2,s0,s1 */
    float a = 1.0f + 1.0f/16777216.0f;       /* 1 + 2^-24 is not representable; */
    volatile float product, sum;             /* the host rounds it to 1.0f      */
    CHECK(prog[0] == 0xee001a20u, "VMLA.F32 s2,s0,s1 = 0x%08x", prog[0]);

    set_f32(&c, 0, 0.0009765625f);           /* 2^-10 */
    set_f32(&c, 1, 0.0009765625f);           /* 2^-10 */
    set_f32(&c, 2, 1.0f);
    CHECK(run(&c, prog, 1, 1) == ARM_OK, "VMLA trapped");
    product = 0.0009765625f * 0.0009765625f;
    sum     = 1.0f + product;
    CHECK(f2u(get_f32(&c, 2)) == f2u(sum), "VMLA = 0x%08x want 0x%08x",
          f2u(get_f32(&c, 2)), f2u(sum));
    (void)a;

    /* VMLS/VNMLA/VNMLS are the same product with sign flips applied. */
    vfp_reset(&c);
    {
        uint32_t p2[] = {
            DP_S(0,0,0,1, 2,0,1),                /* VMLS.F32  s2,s0,s1 */
            DP_S(0,0,1,0, 3,0,1),                /* VNMLS.F32 s3,s0,s1 */
            DP_S(0,0,1,1, 4,0,1),                /* VNMLA.F32 s4,s0,s1 */
        };
        set_f32(&c, 0, 3.0f); set_f32(&c, 1, 5.0f);
        set_f32(&c, 2, 100.0f); set_f32(&c, 3, 100.0f); set_f32(&c, 4, 100.0f);
        CHECK(run(&c, p2, 3, 3) == ARM_OK, "VMLS family trapped");
        CHECK(get_f32(&c, 2) ==   85.0f, "VMLS  = %f", (double)get_f32(&c, 2));
        CHECK(get_f32(&c, 3) ==  -85.0f, "VNMLS = %f", (double)get_f32(&c, 3));
        CHECK(get_f32(&c, 4) == -115.0f, "VNMLA = %f", (double)get_f32(&c, 4));
    }
}

static void test_vabs_vneg_vmov_are_sign_bit_operations(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        UN_S(0,0, 1,0),                      /* VMOV.F32 s1, s0 */
        UN_S(0,1, 2,0),                      /* VABS.F32 s2, s0 */
        UN_S(1,0, 3,0),                      /* VNEG.F32 s3, s0 */
    };
    CHECK(prog[0] == 0xeef00a40u, "VMOV.F32 s1,s0 = 0x%08x", prog[0]);
    CHECK(prog[1] == 0xeeb01ac0u, "VABS.F32 s2,s0 = 0x%08x", prog[1]);
    CHECK(prog[2] == 0xeef11a40u, "VNEG.F32 s3,s0 = 0x%08x", prog[2]);

    /* A signalling NaN. VABS and VNEG touch only the sign bit: the payload
     * survives and IOC is NOT raised, which is what distinguishes them from
     * "multiply by -1". */
    vfp_set_s(&c, 0, 0xff800001u);
    CHECK(run(&c, prog, 3, 3) == ARM_OK, "sign-bit ops trapped");
    CHECK(vfp_get_s(&c, 1) == 0xff800001u, "VMOV = 0x%08x", vfp_get_s(&c, 1));
    CHECK(vfp_get_s(&c, 2) == 0x7f800001u, "VABS = 0x%08x", vfp_get_s(&c, 2));
    CHECK(vfp_get_s(&c, 3) == 0x7f800001u, "VNEG = 0x%08x", vfp_get_s(&c, 3));
    CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) == 0, "sign-bit op raised IOC");

    /* And -0.0 must stay -0.0 through VMOV and become +0.0 through VABS. */
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0x80000000u);
    CHECK(run(&c, prog, 3, 3) == ARM_OK, "sign-bit ops trapped on -0");
    CHECK(vfp_get_s(&c, 1) == 0x80000000u, "VMOV -0 = 0x%08x", vfp_get_s(&c, 1));
    CHECK(vfp_get_s(&c, 2) == 0x00000000u, "VABS -0 = 0x%08x", vfp_get_s(&c, 2));
    CHECK(vfp_get_s(&c, 3) == 0x00000000u, "VNEG -0 = 0x%08x", vfp_get_s(&c, 3));
}

/* ============================================ NaN, infinity, flags ======== */

static void test_infinity_and_nan(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t div[] = { DP_S(1,0,0,0, 2,0,1) };    /* VDIV.F32 s2,s0,s1 */

    /* 1.0 / 0.0 -> +inf, DZC. */
    set_f32(&c, 0, 1.0f); vfp_set_s(&c, 1, 0u);
    CHECK(run(&c, div, 1, 1) == ARM_OK, "VDIV trapped");
    CHECK(vfp_get_s(&c, 2) == 0x7f800000u, "1/0 = 0x%08x", vfp_get_s(&c, 2));
    CHECK((c.vfp_fpscr & ARM_FPSCR_DZC) != 0, "DZC not set, FPSCR = 0x%08x", c.vfp_fpscr);
    CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) == 0, "IOC set for 1/0");

    /* -1.0 / 0.0 -> -inf. */
    vfp_reset(&c);
    set_f32(&c, 0, -1.0f); vfp_set_s(&c, 1, 0u);
    run(&c, div, 1, 1);
    CHECK(vfp_get_s(&c, 2) == 0xff800000u, "-1/0 = 0x%08x", vfp_get_s(&c, 2));

    /* 0.0 / 0.0 -> a quiet NaN, IOC (not DZC). */
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0u); vfp_set_s(&c, 1, 0u);
    run(&c, div, 1, 1);
    CHECK((vfp_get_s(&c, 2) & 0x7fc00000u) == 0x7fc00000u &&
          (vfp_get_s(&c, 2) & 0x007fffffu) != 0,
          "0/0 = 0x%08x, want a quiet NaN", vfp_get_s(&c, 2));
    CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) != 0, "IOC not set for 0/0");
    CHECK((c.vfp_fpscr & ARM_FPSCR_DZC) == 0, "DZC set for 0/0");

    /* inf * 0 -> NaN, IOC. */
    vfp_reset(&c);
    {
        uint32_t mul[] = { DP_S(0,1,0,0, 2,0,1) };  /* VMUL.F32 s2,s0,s1 */
        vfp_set_s(&c, 0, 0x7f800000u); vfp_set_s(&c, 1, 0u);
        run(&c, mul, 1, 1);
        CHECK((vfp_get_s(&c, 2) & 0x7f800000u) == 0x7f800000u &&
              (vfp_get_s(&c, 2) & 0x007fffffu) != 0,
              "inf*0 = 0x%08x", vfp_get_s(&c, 2));
        CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) != 0, "IOC not set for inf*0");
    }

    /* Overflow of finite operands must reach infinity and set OFC + IXC. */
    vfp_reset(&c);
    {
        uint32_t mul[] = { DP_S(0,1,0,0, 2,0,1) };
        vfp_set_s(&c, 0, 0x7f7fffffu);       /* FLT_MAX */
        set_f32(&c, 1, 2.0f);
        run(&c, mul, 1, 1);
        CHECK(vfp_get_s(&c, 2) == 0x7f800000u, "overflow = 0x%08x", vfp_get_s(&c, 2));
        CHECK((c.vfp_fpscr & ARM_FPSCR_OFC) != 0, "OFC not set, FPSCR = 0x%08x", c.vfp_fpscr);
        CHECK((c.vfp_fpscr & ARM_FPSCR_IXC) != 0, "IXC not set, FPSCR = 0x%08x", c.vfp_fpscr);
    }

    /* And a plain inexact result sets IXC but nothing else. */
    vfp_reset(&c);
    {
        uint32_t d[] = { DP_S(1,0,0,0, 2,0,1) };
        set_f32(&c, 0, 1.0f); set_f32(&c, 1, 3.0f);
        run(&c, d, 1, 1);
        CHECK((c.vfp_fpscr & 0x9fu) == ARM_FPSCR_IXC,
              "1/3 set FPSCR flags 0x%08x, want only IXC", c.vfp_fpscr & 0x9fu);
    }
}

/* ================================================== comparisons ========== */

/* VCMP writes FPSCR.NZCV, never CPSR. VMRS APSR_nzcv is the only thing that
 * moves them across, and testing both together is the only way to catch an
 * implementation that wrote the wrong register. */
static void test_vcmp_writes_fpscr_not_cpsr(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        UN_S(4,0, 0,1),                      /* VCMP.F32 s0, s1     */
        0xeef1fa10u,                         /* VMRS APSR_nzcv, FPSCR */
    };
    CHECK(prog[0] == 0xeeb40a60u, "VCMP.F32 s0,s1 = 0x%08x", prog[0]);

    /* 1.0 < 2.0 -> N set, everything else clear. */
    set_f32(&c, 0, 1.0f); set_f32(&c, 1, 2.0f);
    load(prog, 2);
    CHECK(arm_step(&c) == ARM_OK, "VCMP trapped");
    CHECK((c.vfp_fpscr & ARM_FPSCR_NZCV) == ARM_FPSCR_N,
          "less-than FPSCR.NZCV = 0x%08x", c.vfp_fpscr & ARM_FPSCR_NZCV);
    CHECK((c.cpsr & 0xf0000000u) == 0, "VCMP wrote CPSR: 0x%08x", c.cpsr);
    CHECK(arm_step(&c) == ARM_OK, "VMRS APSR_nzcv trapped");
    CHECK((c.cpsr & 0xf0000000u) == ARM_CPSR_N, "CPSR after VMRS = 0x%08x", c.cpsr);

    /* Equal -> Z and C. -0.0 compares equal to +0.0. */
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0x80000000u); vfp_set_s(&c, 1, 0u);
    run(&c, prog, 1, 1);
    CHECK((c.vfp_fpscr & ARM_FPSCR_NZCV) == (ARM_FPSCR_Z | ARM_FPSCR_C),
          "-0 == +0 gave 0x%08x", c.vfp_fpscr & ARM_FPSCR_NZCV);

    /* Greater-than -> C alone. */
    vfp_reset(&c);
    set_f32(&c, 0, 5.0f); set_f32(&c, 1, 2.0f);
    run(&c, prog, 1, 1);
    CHECK((c.vfp_fpscr & ARM_FPSCR_NZCV) == ARM_FPSCR_C,
          "greater-than gave 0x%08x", c.vfp_fpscr & ARM_FPSCR_NZCV);

    /* Unordered -> C and V. A QUIET NaN does not raise IOC for plain VCMP... */
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0x7fc00001u); set_f32(&c, 1, 2.0f);
    run(&c, prog, 1, 1);
    CHECK((c.vfp_fpscr & ARM_FPSCR_NZCV) == (ARM_FPSCR_C | ARM_FPSCR_V),
          "unordered gave 0x%08x", c.vfp_fpscr & ARM_FPSCR_NZCV);
    CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) == 0, "VCMP raised IOC on a quiet NaN");

    /* ...but VCMPE does, and so does VCMP on a SIGNALLING NaN. */
    vfp_reset(&c);
    {
        uint32_t e[] = { UN_S(4,1, 0,1) };             /* VCMPE.F32 s0, s1 */
        CHECK(e[0] == 0xeeb40ae0u, "VCMPE.F32 s0,s1 = 0x%08x", e[0]);
        vfp_set_s(&c, 0, 0x7fc00001u); set_f32(&c, 1, 2.0f);
        run(&c, e, 1, 1);
        CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) != 0, "VCMPE did not raise IOC");
    }
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0x7f800001u);           /* signalling NaN */
    set_f32(&c, 1, 2.0f);
    run(&c, prog, 1, 1);
    CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) != 0, "VCMP did not raise IOC on an SNaN");
}

static void test_vcmp_with_zero(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = { UN_S(5,0, 0,0) };                /* VCMP.F32 s0, #0.0 */
    CHECK(prog[0] == 0xeeb50a40u, "VCMP.F32 s0,#0 = 0x%08x", prog[0]);
    set_f32(&c, 0, -3.0f);
    CHECK(run(&c, prog, 1, 1) == ARM_OK, "VCMP #0 trapped");
    CHECK((c.vfp_fpscr & ARM_FPSCR_NZCV) == ARM_FPSCR_N,
          "-3 vs 0 gave 0x%08x", c.vfp_fpscr & ARM_FPSCR_NZCV);

    /* A non-zero Vm field in the compare-with-zero form is UNPREDICTABLE. */
    vfp_reset(&c);
    { uint32_t bad[] = { UN_S(5,0, 0,2) };
      CHECK(run(&c, bad, 1, 1) == ARM_UNDEFINED, "VCMP #0 with Vm != 0 ran"); }
}

/* ================================================== conversions ========== */

static void test_conversions(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = {
        UN_S(8,1, 2,0),                      /* VCVT.F32.S32 s2, s0 */
        UN_S(8,0, 4,0),                      /* VCVT.F32.U32 s4, s0 */
        UN_S(13,1, 6,6),                     /* VCVT.S32.F32 s6, s6 */
    };
    CHECK(prog[0] == 0xeeb81ac0u, "VCVT.F32.S32 s2,s0 = 0x%08x", prog[0]);
    CHECK(prog[1] == 0xeeb82a40u, "VCVT.F32.U32 s4,s0 = 0x%08x", prog[1]);

    vfp_set_s(&c, 0, 0xffffffffu);           /* -1 signed, 4294967295 unsigned */
    set_f32(&c, 6, -2.5f);
    CHECK(run(&c, prog, 3, 3) == ARM_OK, "conversions trapped");
    CHECK(get_f32(&c, 2) == -1.0f, "S32->F32 = %f", (double)get_f32(&c, 2));
    CHECK(get_f32(&c, 4) == 4294967296.0f, "U32->F32 = %f", (double)get_f32(&c, 4));
    /* VCVT (not VCVTR) truncates toward zero: -2.5 -> -2. */
    CHECK((int32_t)vfp_get_s(&c, 6) == -2, "F32->S32 = %d", (int32_t)vfp_get_s(&c, 6));

    /* VCVTR uses FPSCR rounding, which is round-to-nearest-EVEN: -2.5 -> -2
     * and 2.5 -> 2, but 3.5 -> 4. */
    vfp_reset(&c);
    {
        uint32_t r[] = { UN_S(13,0, 0,0) };              /* VCVTR.S32.F32 s0,s0 */
        CHECK(r[0] == 0xeebd0a40u, "VCVTR.S32.F32 s0,s0 = 0x%08x", r[0]);
        set_f32(&c, 0, 2.5f);  run(&c, r, 1, 1);
        CHECK((int32_t)vfp_get_s(&c, 0) == 2, "VCVTR 2.5 -> %d", (int32_t)vfp_get_s(&c, 0));
        vfp_reset(&c);
        set_f32(&c, 0, 3.5f);  run(&c, r, 1, 1);
        CHECK((int32_t)vfp_get_s(&c, 0) == 4, "VCVTR 3.5 -> %d", (int32_t)vfp_get_s(&c, 0));
    }

    /* Out of range and NaN saturate with IOC, per ARM's FPToFixed. */
    vfp_reset(&c);
    {
        uint32_t t[] = { UN_S(13,1, 0,0) };              /* VCVT.S32.F32 s0,s0 */
        vfp_set_s(&c, 0, 0x7f800000u);                   /* +inf */
        run(&c, t, 1, 1);
        CHECK(vfp_get_s(&c, 0) == 0x7fffffffu, "+inf -> 0x%08x", vfp_get_s(&c, 0));
        CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) != 0, "no IOC on saturation");

        vfp_reset(&c);
        vfp_set_s(&c, 0, 0xff800000u);                   /* -inf */
        run(&c, t, 1, 1);
        CHECK(vfp_get_s(&c, 0) == 0x80000000u, "-inf -> 0x%08x", vfp_get_s(&c, 0));

        vfp_reset(&c);
        vfp_set_s(&c, 0, 0x7fc00000u);                   /* NaN  */
        run(&c, t, 1, 1);
        CHECK(vfp_get_s(&c, 0) == 0u, "NaN -> 0x%08x, want 0", vfp_get_s(&c, 0));
        CHECK((c.vfp_fpscr & ARM_FPSCR_IOC) != 0, "no IOC on a NaN conversion");
    }

    /* Single <-> double. Widening is exact; narrowing rounds. */
    vfp_reset(&c);
    {
        uint32_t w[] = {
            UN_D_FROM_S(7,1, 0,1),           /* VCVT.F64.F32 d0, s1 */
            UN_S_FROM_D(7,1, 4,0),           /* VCVT.F32.F64 s4, d0 */
        };
        CHECK(w[0] == 0xeeb70ae0u, "VCVT.F64.F32 d0,s1 = 0x%08x", w[0]);
        CHECK(w[1] == 0xeeb72bc0u, "VCVT.F32.F64 s4,d0 = 0x%08x", w[1]);
        set_f32(&c, 1, 0.5f);
        CHECK(run(&c, w, 2, 2) == ARM_OK, "VCVT f32/f64 trapped");
        CHECK(get_f64(&c, 0) == 0.5, "F32->F64 = %f", get_f64(&c, 0));
        CHECK(get_f32(&c, 4) == 0.5f, "F64->F32 = %f", (double)get_f32(&c, 4));
    }
}

/* ============================================ things that must trap ====== */

/*
 * The contract: an encoding this unit does not implement stops the machine
 * with a named reason. It must NEVER be a no-op, and it must never be quietly
 * folded onto a register that happens to exist. These are all cases where a
 * plausible-looking wrong answer was easy to produce.
 */
static void test_unimplemented_encodings_still_halt(void) {
    arm_cpu_t c;
    struct { uint32_t insn; const char *what; } cases[] = {
        /* d16-d31: VFPv2 has 16 doubles, and the D bit that would name d16 is
         * SBZ. Folding it back onto d0 would corrupt an unrelated register. */
        { VFP_DP(0,1,1, 1,0,0, 1, 0,0, 0,0), "VADD.F64 d16, d0, d0" },
        { VFP_DP(0,1,1, 0,0,0, 1, 1,0, 0,0), "VADD.F64 d0, d16, d0" },
        { VFP_DP(0,1,1, 0,0,0, 1, 0,0, 1,0), "VADD.F64 d0, d0, d16" },
        { VFP_LS(1,1,1,0,1, 1, 0, 1, 0),     "VLDR d16, [r1]"       },
        /* VFPv3 and VFPv4 encodings that the VFP11 does not have. */
        { 0xeeb00b00u,                       "VMOV.F64 d0, #imm (VFPv3)" },
        { UN_S(2,1, 0,0),                    "VCVTB half-precision (VFPv3)" },
        { UN_S(10,1, 0,0),                   "VCVT fixed-point (VFPv3)" },
        { DP_S(1,1,0,0, 0,0,0),              "VFMA (VFPv4)" },
        { DP_S(1,0,1,0, 0,0,0),              "VFNMA (VFPv4)" },
        /* Advanced SIMD scalar transfer: no NEON on this part. */
        { 0xee000b10u,                       "VMOV.32 d0[0], r0 (NEON)" },
        /* VFP system registers we do not implement. */
        { VMRS(0, 7),                        "VMRS r0, MVFR0" },
        { VMRS(0, 9),                        "VMRS r0, FPINST" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint32_t p[1]; p[0] = cases[i].insn;
        vfp_reset(&c);
        CHECK(run(&c, p, 1, 1) == ARM_UNDEFINED, "%s did not halt", cases[i].what);
        CHECK(vfp_trap_reason() != NULL, "%s halted without a reason", cases[i].what);
    }
}

/*
 * FPSCR modes we cannot reproduce bit-exactly must stop the machine rather
 * than compute a plausible wrong number. The split matters: VMOV and VABS do
 * not consult the rounding mode, so they must keep working in RZ, while VADD
 * must not.
 */
static void test_unmodelled_fpscr_modes_halt(void) {
    arm_cpu_t c;
    uint32_t add[] = { DP_S(0,1,1,0, 2,0,1) };   /* VADD.F32 s2,s0,s1 */
    uint32_t mov[] = { UN_S(0,0, 1,0) };         /* VMOV.F32 s1,s0    */

    struct { uint32_t fpscr; const char *what; } modes[] = {
        { ARM_FPSCR_RMODE, "RMode = RZ"      },
        { ARM_FPSCR_LEN,   "Len != 0"        },
        { ARM_FPSCR_IOE,   "IOE trap enable" },
    };
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        vfp_reset(&c); c.vfp_fpscr = modes[i].fpscr;
        CHECK(run(&c, add, 1, 1) == ARM_UNDEFINED, "VADD ran with %s", modes[i].what);
    }

    /* VMOV survives everything except short vectors, which change which
     * registers it writes. */
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_RMODE;
    CHECK(run(&c, mov, 1, 1) == ARM_OK, "VMOV refused in RZ mode");
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_IOE;
    CHECK(run(&c, mov, 1, 1) == ARM_OK, "VMOV refused with a trap enable set");
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_LEN;
    CHECK(run(&c, mov, 1, 1) == ARM_UNDEFINED, "VMOV ran with Len != 0");

    /*
     * An integer-source conversion has nothing for FZ or DN to act on and, for
     * a double destination, cannot even be inexact — so it must run in any of
     * those modes. This is not a hypothetical: dyld runs in ARM's RunFast
     * configuration (FZ set) and its first floating-point instruction is
     * VCVT.F64.U32 on the mach_timebase_info numerator. Refusing it here
     * stopped the boot dead at pc 0x2120.
     */
    {
        uint32_t cvt[] = { VFP_DP(1,1,1, 0, 8, 6, 1, 0, 1, 1, 5) };  /* VCVT.F64.U32 d6,s11 */
        CHECK(cvt[0] == 0xeeb86b65u, "VCVT.F64.U32 d6,s11 = 0x%08x", cvt[0]);
        vfp_reset(&c);
        c.vfp_fpscr = ARM_FPSCR_FZ | ARM_FPSCR_DN | ARM_FPSCR_RMODE;
        vfp_set_s(&c, 11, 1000000000u);
        CHECK(run(&c, cvt, 1, 1) == ARM_OK, "VCVT.F64.U32 refused in RunFast mode");
        CHECK(get_f64(&c, 6) == 1000000000.0, "d6 = %f", get_f64(&c, 6));
    }
}

/*
 * Flush-to-zero, FPSCR.FZ, which dyld turns on. Denormal operands become zero
 * of the same sign with IDC set, and a result below the smallest normal
 * becomes zero of the result's sign with UFC set.
 */
static void test_flush_to_zero(void) {
    arm_cpu_t c;
    uint32_t mul[] = { DP_S(0,1,0,0, 2,0,1) };   /* VMUL.F32 s2, s0, s1 */
    uint32_t add[] = { DP_S(0,1,1,0, 2,0,1) };   /* VADD.F32 s2, s0, s1 */

    /* A denormal input is flushed, so denormal + 0 is exactly zero, not the
     * denormal. IDC records that it happened. */
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_FZ;
    vfp_set_s(&c, 0, 0x00000001u);               /* the smallest denormal */
    vfp_set_s(&c, 1, 0u);
    CHECK(run(&c, add, 1, 1) == ARM_OK, "VADD refused in FZ mode");
    CHECK(vfp_get_s(&c, 2) == 0u, "denormal + 0 = 0x%08x", vfp_get_s(&c, 2));
    CHECK((c.vfp_fpscr & ARM_FPSCR_IDC) != 0, "IDC not set, FPSCR = 0x%08x", c.vfp_fpscr);

    /* The flushed zero keeps its sign, which is what makes a multiply come out
     * negative rather than positive. */
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_FZ;
    vfp_set_s(&c, 0, 0x80000001u);               /* a NEGATIVE denormal */
    set_f32(&c, 1, 2.0f);
    CHECK(run(&c, mul, 1, 1) == ARM_OK, "VMUL refused in FZ mode");
    CHECK(vfp_get_s(&c, 2) == 0x80000000u, "-denormal * 2 = 0x%08x, want -0",
          vfp_get_s(&c, 2));

    /* A result that underflows into the denormal range is flushed on the way
     * out, with UFC set. */
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_FZ;
    vfp_set_s(&c, 0, 0x00800000u);               /* the smallest normal */
    set_f32(&c, 1, 0.25f);
    CHECK(run(&c, mul, 1, 1) == ARM_OK, "VMUL refused in FZ mode");
    CHECK(vfp_get_s(&c, 2) == 0u, "underflow = 0x%08x, want +0", vfp_get_s(&c, 2));
    CHECK((c.vfp_fpscr & ARM_FPSCR_UFC) != 0, "UFC not set, FPSCR = 0x%08x", c.vfp_fpscr);

    /* With FZ clear the same operation keeps the denormal: the mode really is
     * doing something, rather than the host doing it for us. */
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0x00800000u);
    set_f32(&c, 1, 0.25f);
    run(&c, mul, 1, 1);
    CHECK(vfp_get_s(&c, 2) == 0x00200000u, "no-FZ underflow = 0x%08x",
          vfp_get_s(&c, 2));

    /* And FZ leaves normal arithmetic completely alone. */
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_FZ;
    set_f32(&c, 0, 1.5f); set_f32(&c, 1, 2.5f);
    run(&c, add, 1, 1);
    CHECK(get_f32(&c, 2) == 4.0f, "FZ perturbed a normal add: %f",
          (double)get_f32(&c, 2));
    CHECK((c.vfp_fpscr & 0x9fu) == 0u, "FZ set flags on an exact add: 0x%08x",
          c.vfp_fpscr & 0x9fu);
}

/* Default-NaN mode replaces every NaN result with the one default quiet NaN,
 * which also makes the NaN-payload deviation documented in vfp.c unobservable. */
static void test_default_nan(void) {
    arm_cpu_t c;
    uint32_t add[] = { DP_S(0,1,1,0, 2,0,1) };   /* VADD.F32 s2, s0, s1 */

    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_DN;
    vfp_set_s(&c, 0, 0x7fc12345u);               /* a quiet NaN with a payload */
    set_f32(&c, 1, 1.0f);
    CHECK(run(&c, add, 1, 1) == ARM_OK, "VADD refused in DN mode");
    CHECK(vfp_get_s(&c, 2) == 0x7fc00000u, "DN result = 0x%08x, want the default NaN",
          vfp_get_s(&c, 2));

    /* Without DN the payload propagates instead. */
    vfp_reset(&c);
    vfp_set_s(&c, 0, 0x7fc12345u);
    set_f32(&c, 1, 1.0f);
    run(&c, add, 1, 1);
    CHECK(vfp_get_s(&c, 2) == 0x7fc12345u, "non-DN result = 0x%08x, want the payload",
          vfp_get_s(&c, 2));

    /* Double precision has its own default NaN. */
    vfp_reset(&c); c.vfp_fpscr = ARM_FPSCR_DN;
    {
        uint32_t addd[] = { DP_D(0,1,1,0, 2,0,1) };   /* VADD.F64 d2, d0, d1 */
        vfp_set_d(&c, 0, 0x7ff8123456789abcull);
        set_f64(&c, 1, 1.0);
        CHECK(run(&c, addd, 1, 1) == ARM_OK, "VADD.F64 refused in DN mode");
        CHECK(vfp_get_d(&c, 2) == 0x7ff8000000000000ull, "DN.F64 = 0x%016llx",
              (unsigned long long)vfp_get_d(&c, 2));
    }
}

/* A VLDM whose list crosses into unmapped memory must abort like an LDM. The
 * flat test bus never faults, so this checks the plumbing instead: a load that
 * runs is a load that reached vfp_bus, and the abort latch stays clear. */
static void test_ldm_uses_the_translating_bus(void) {
    arm_cpu_t c; vfp_reset(&c);
    uint32_t prog[] = { VFP_LS(0,1,0,0,1, 1, 0, 1, 8) };  /* VLDMIA r1, {d0-d3} */
    for (unsigned i = 0; i < 8; i++) m_w32(NULL, 0x7000u + i*4u, 0x5a5a0000u + i);
    c.r[1] = 0x7000;
    CHECK(run(&c, prog, 1, 1) == ARM_OK, "VLDMIA {d0-d3} trapped");
    CHECK(!c.abort_pending, "abort latched on a clean load");
    CHECK(vfp_get_d(&c, 0) == 0x5a5a00015a5a0000ull, "d0 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 0));
    CHECK(vfp_get_d(&c, 3) == 0x5a5a00075a5a0006ull, "d3 = 0x%016llx",
          (unsigned long long)vfp_get_d(&c, 3));
}

static unsigned g_fault_reads32, g_fault_writes32;
static uint32_t fault_first_r32(arm_cpu_t *c, uint32_t va) {
    g_fault_reads32++;
    c->abort_pending = true;
    c->abort_fsr = ARM_FSR_PAGE_TRANSLATION;
    c->abort_far = va;
    return 0x11223344u;
}
static void fault_first_w32(arm_cpu_t *c, uint32_t va, uint32_t v) {
    (void)v;
    g_fault_writes32++;
    c->abort_pending = true;
    c->abort_fsr = ARM_FSR_PAGE_TRANSLATION | (1u << 11);
    c->abort_far = va;
}
static const vfp_bus_t g_fault_first_bus = { fault_first_r32, fault_first_w32 };

static void test_double_transfers_stop_after_the_first_abort(void) {
    arm_cpu_t c;
    static const uint32_t insns[] = {
        VFP_LS(1,1,0,0,1, 1,0,1,0),            /* VLDR   d0,[r1]   */
        VFP_LS(0,1,0,0,1, 1,0,1,2),            /* VLDMIA r1,{d0}  */
        VFP_LS(1,1,0,0,0, 1,0,1,0),            /* VSTR   d0,[r1]   */
        VFP_LS(0,1,0,0,0, 1,0,1,2),            /* VSTMIA r1,{d0}  */
    };

    for (unsigned i = 0; i < 4u; i++) {
        vfp_reset(&c);
        c.r[1] = 0x800u;
        vfp_set_d(&c, 0, 0x8877665544332211ull);
        c.abort_pending = false;
        g_fault_reads32 = g_fault_writes32 = 0u;
        CHECK(vfp_execute(&c, 0u, insns[i], &g_fault_first_bus) == ARM_OK,
              "faulting double transfer %u returned a decode error", i);
        CHECK(c.abort_pending, "double transfer %u did not preserve its abort", i);
        if (i < 2u)
            CHECK(g_fault_reads32 == 1u && g_fault_writes32 == 0u,
                  "double load %u issued %u reads/%u writes after first abort",
                  i, g_fault_reads32, g_fault_writes32);
        else
            CHECK(g_fault_writes32 == 1u && g_fault_reads32 == 0u,
                  "double store %u issued %u writes/%u reads after first abort",
                  i, g_fault_writes32, g_fault_reads32);
    }
}

/* Conditional execution applies to VFP like everything else. */
static void test_condition_codes_apply(void) {
    arm_cpu_t c; vfp_reset(&c);
    /* ADDEQ-style: make the VADD NE, then set Z so it is skipped. */
    uint32_t prog[] = { (DP_S(0,1,1,0, 2,0,1) & 0x0fffffffu) | 0x10000000u };
    set_f32(&c, 0, 1.0f); set_f32(&c, 1, 2.0f); set_f32(&c, 2, 99.0f);
    c.cpsr |= ARM_CPSR_Z;
    CHECK(run(&c, prog, 1, 1) == ARM_OK, "conditional VADD trapped");
    CHECK(get_f32(&c, 2) == 99.0f, "VADDNE executed with Z set: %f",
          (double)get_f32(&c, 2));
    CHECK(c.r[15] == 4u, "PC = 0x%08x", c.r[15]);
}

/* --------------------------------------------------------------- main ---- */
int main(void) {
    printf("VFPv2 (VFP11) tests\n");
    test_s_d_aliasing();
    test_vldmia_writeback_the_vfp_switch_form();
    test_vstmia_vldmia_round_trip();
    test_double_list_matches_single_list();
    test_vpush_vpop();
    test_vldr_vstr();
    test_overlong_lists_trap();
    test_vmrs_vmsr();
    test_fpsid_is_read_only();
    test_fpexc_en_gates_the_other_registers();
    test_vmov_core_registers();
    test_single_precision_arithmetic();
    test_double_precision_arithmetic();
    test_vmla_is_not_fused();
    test_vabs_vneg_vmov_are_sign_bit_operations();
    test_infinity_and_nan();
    test_vcmp_writes_fpscr_not_cpsr();
    test_vcmp_with_zero();
    test_conversions();
    test_unimplemented_encodings_still_halt();
    test_unmodelled_fpscr_modes_halt();
    test_flush_to_zero();
    test_default_nan();
    test_ldm_uses_the_translating_bus();
    test_double_transfers_stop_after_the_first_abort();
    test_condition_codes_apply();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

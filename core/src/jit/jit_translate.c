/*
 * iOS3-VM — ARMv6 -> arm64 block translator.
 *
 * Translates a straight-line run of ARM or Thumb instructions into arm64,
 * stopping at the first thing it does not handle and leaving that instruction
 * to the interpreter. See core/include/jit.h for the structural rule and
 * docs/dynarec.md for the design this implements.
 *
 * The file is in two halves. This one is ARM (docs/dynarec.md's J2/J3); the
 * Thumb half begins at the "Thumb" banner below and carries its own commentary
 * on the flag rule and the ARM/Thumb state boundary, because those are where
 * it differs. A block is one instruction set or the other, never both: the
 * block key records which (§3.3) and every state change ends a block (§3.2).
 *
 * ============================ FLAG MAPPING ================================
 *
 * Guest N/Z/C/V live in the host PSTATE.NZCV between guest instructions and
 * across block boundaries. The rest of CPSR (I, F, T, Q, mode) is memory-only
 * and is never touched by emitted code. A block's prologue does
 * `msr nzcv, cpsr` and its epilogue merges bits 31:28 back into cpu->cpsr,
 * leaving Q and everything else untouched.
 *
 *   Exact, no fixup needed (docs/dynarec.md §5.1):
 *     ADDS/ADC/CMN   -> A64 ADDS/ADCS/CMN     (same carry convention)
 *     SUBS/SBC/CMP   -> A64 SUBS/SBCS/CMP     (C set == no borrow, same as A32)
 *     RSB/RSC        -> the same, with the operands swapped
 *     condition codes EQ..LE -> identical encodings, so a guest conditional
 *                               instruction is one B.<inverse> over its body
 *
 *   NOT exact, fixed up explicitly (docs/dynarec.md §5.2(1)):
 *     A32 ANDS/ORRS/EORS/BICS/MOVS/MVNS/TST/TEQ set N and Z from the result,
 *     set C from the *barrel-shifter* carry-out, and PRESERVE V.
 *     A64 ANDS sets N and Z but forces C = 0 and V = 0 — wrong in two flags.
 *     emit_logic_flags() below therefore recomputes N/Z with ANDS and splices
 *     the correct C and V back in with MRS/UBFX/BFI/MSR.
 *
 *   DEFERRED — these fall back to the interpreter rather than being guessed:
 *     - register-specified shift amounts (`LSL Rm, Rs`). A32 treats Rs == 32
 *       as "shift by 32" while A64's LSLV is modulo 32, i.e. the opposite
 *       answer, and Rs == 0 leaves both value and carry untouched
 *       (docs/dynarec.md §5.2(2)).
 *     - RRX (`ROR #0`), and `LSR #0`/`ASR #0` which mean #32 (§5.2(3)).
 *     - any S-setting logical instruction whose shifter carry-out is not a
 *       translate-time constant, i.e. a register operand with a non-zero
 *       immediate shift. The rotated-immediate case *is* a constant and is
 *       folded (§5.2(4)); the register case is not, so it is not attempted.
 *       (Thumb's LSLS/LSRS/ASRS by an immediate cannot be deferred this way —
 *       they always set C and are 2.48% of retired instructions — so the Thumb
 *       half computes the carry-out into a register instead. See there.)
 *     - the Q flag: no saturating instruction is translated, so nothing here
 *       can set it. QADD/QSUB/QDADD/QDSUB fall back.
 *     - lazy/deferred flag evaluation, deliberately: the interpreter is eager
 *       and exact, and every exception entry observes CPSR (§5.3).
 *
 * ================= WHY THE ABORT PATH NEEDS NO ABORT CODE =================
 *
 * The load/store helpers below are pure with respect to architectural state:
 * on a translation fault they read nothing, write nothing, and report the
 * fault. The emitted code then exits the block with JIT_EXIT_ABORT and the
 * *interpreter* re-executes that one instruction, faults identically, and
 * takes the data abort through take_exception(). Because no translated access
 * has a writeback form (ARM is P == 1, W == 0 only; no Thumb 16-bit
 * single-access encoding writes back at all) and the destination register is
 * written only after the fault check, the guest has observed nothing at that
 * point, so re-execution is exact. The consequence is that
 * the base-restored abort model and the exception-return alignment rule
 * (docs/dynarec.md §7.3, §7.4 — the bug that once unlocked a mutex at address
 * 1) exist in exactly one place in this codebase, forever.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "jit.h"
#include "a64_emit.h"
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------- deny mask */

static uint32_t g_deny;
void     jit_set_deny(uint32_t mask) { g_deny = mask; }
uint32_t jit_get_deny(void)          { return g_deny; }

/* --------------------------------------------------------- CPU offsets -- */

#define OFF_R(n)   ((uint32_t)(offsetof(arm_cpu_t, r[0]) + 4u * (unsigned)(n)))
#define OFF_CPSR   ((uint32_t)offsetof(arm_cpu_t, cpsr))
#define OFF_CYCLES ((uint32_t)offsetof(arm_cpu_t, cycles))

/* ------------------------------------------------------- memory helpers --
 * Mirror the interpreter's mem_rN_as()/mem_wN_as() exactly, minus the abort
 * latching: a fault is reported to the caller instead, and the caller bails
 * out to the interpreter (see the header comment).
 *
 * Loads return the value in bits 31:0 and set bit 32 on a fault; stores
 * return non-zero on a fault. That keeps the fault test to a single TBNZ/CBNZ
 * with no out-parameter.
 */
static bool jit_priv(const arm_cpu_t *c) {
    return (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
}

#define JIT_PAGE_MASK 0xfffu

/* Resolve every page touched by a crossing access before performing any bus
 * transaction. The JIT replays a faulting instruction in the interpreter, so
 * an early MMIO read or write here would otherwise happen twice. */
static bool jit_pretranslate_crossing(arm_cpu_t *c, uint32_t va, unsigned n,
                                      arm_access_t access, uint32_t *pas) {
    uint32_t page_pa = 0;
    for (unsigned i = 0; i < n; i++) {
        uint32_t a = va + i;
        if (i == 0u || (a & JIT_PAGE_MASK) == 0u) {
            uint32_t pa;
            if (arm_mmu_translate(c, a, access, jit_priv(c), &pa)) return false;
            page_pa = pa & ~JIT_PAGE_MASK;
        }
        pas[i] = page_pa | (a & JIT_PAGE_MASK);
    }
    return true;
}

uint64_t jit_mem_load32(arm_cpu_t *c, uint32_t va) {
    uint32_t pa;
    /* Same page-crossing split as the interpreter's mem_r32: a word straddling
     * a 4 KB boundary lives in two pages that need translating separately.
     * Preflight both translations so a second-page fault cannot double an
     * MMIO read when the interpreter replays the instruction. */
    if (((va & JIT_PAGE_MASK) + 4u) > JIT_PAGE_MASK + 1u) {
        uint32_t val = 0, pas[4];
        if (!jit_pretranslate_crossing(c, va, 4u, ARM_ACCESS_READ, pas))
            return (uint64_t)1 << 32;
        for (unsigned i = 0; i < 4u; i++) {
            val |= (uint32_t)c->bus->read8(c->bus->ctx, pas[i]) << (8u * i);
        }
        return val;
    }
    if (arm_mmu_translate(c, va, ARM_ACCESS_READ, jit_priv(c), &pa))
        return (uint64_t)1 << 32;
    return c->bus->read32(c->bus->ctx, pa);
}
uint64_t jit_mem_load16(arm_cpu_t *c, uint32_t va) {
    uint32_t pa;
    if (((va & JIT_PAGE_MASK) + 2u) > JIT_PAGE_MASK + 1u) {
        uint32_t pas[2], val = 0;
        if (!jit_pretranslate_crossing(c, va, 2u, ARM_ACCESS_READ, pas))
            return (uint64_t)1 << 32;
        for (unsigned i = 0; i < 2u; i++)
            val |= (uint32_t)c->bus->read8(c->bus->ctx, pas[i]) << (8u * i);
        return val;
    }
    if (arm_mmu_translate(c, va, ARM_ACCESS_READ, jit_priv(c), &pa))
        return (uint64_t)1 << 32;
    return c->bus->read16(c->bus->ctx, pa);
}
static uint64_t jit_helper_load8(arm_cpu_t *c, uint32_t va) {
    uint32_t pa;
    if (arm_mmu_translate(c, va, ARM_ACCESS_READ, jit_priv(c), &pa))
        return (uint64_t)1 << 32;
    return c->bus->read8(c->bus->ctx, pa);
}
uint32_t jit_mem_store32(arm_cpu_t *c, uint32_t va, uint32_t val) {
    uint32_t pa;
    if (((va & JIT_PAGE_MASK) + 4u) > JIT_PAGE_MASK + 1u) {
        uint32_t pas[4];
        if (!jit_pretranslate_crossing(c, va, 4u, ARM_ACCESS_WRITE, pas))
            return 1u;
        for (unsigned i = 0; i < 4u; i++)
            c->bus->write8(c->bus->ctx, pas[i], (uint8_t)(val >> (8u * i)));
        return 0u;
    }
    if (arm_mmu_translate(c, va, ARM_ACCESS_WRITE, jit_priv(c), &pa)) return 1u;
    c->bus->write32(c->bus->ctx, pa, val);
    return 0u;
}
uint32_t jit_mem_store16(arm_cpu_t *c, uint32_t va, uint32_t val) {
    uint32_t pa;
    if (((va & JIT_PAGE_MASK) + 2u) > JIT_PAGE_MASK + 1u) {
        uint32_t pas[2];
        if (!jit_pretranslate_crossing(c, va, 2u, ARM_ACCESS_WRITE, pas))
            return 1u;
        for (unsigned i = 0; i < 2u; i++)
            c->bus->write8(c->bus->ctx, pas[i], (uint8_t)(val >> (8u * i)));
        return 0u;
    }
    if (arm_mmu_translate(c, va, ARM_ACCESS_WRITE, jit_priv(c), &pa)) return 1u;
    c->bus->write16(c->bus->ctx, pa, (uint16_t)val);
    return 0u;
}
static uint32_t jit_helper_store8(arm_cpu_t *c, uint32_t va, uint32_t val) {
    uint32_t pa;
    if (arm_mmu_translate(c, va, ARM_ACCESS_WRITE, jit_priv(c), &pa)) return 1u;
    c->bus->write8(c->bus->ctx, pa, (uint8_t)val);
    return 0u;
}

/* ------------------------------------------------------ translator state */

typedef struct {
    arm_cpu_t  *cpu;
    a64_emit_t  e;
    uint32_t    pc;       /* VA of the instruction being translated          */
    unsigned    index;    /* guest instructions retired before this one      */
    bool        priv;
    bool        thumb;    /* block is Thumb; fixed for the whole block (§3.3) */
    size_t      epi[2 * JIT_MAX_INSNS + 8];  /* branches awaiting the epilogue */
    unsigned    n_epi;
} jit_t;

/* Worst-case host words for one guest instruction, used to reserve headroom
 * so the emitter never overflows mid-instruction. */
#define JIT_INSN_HEADROOM 40u
#define JIT_EPILOGUE_WORDS 32u

/* --------------------------------------------------- register plumbing -- */

/* The pinned host register for a guest register, or 32 if it lives in memory. */
static unsigned host_of(unsigned g) {
    if (g <= 7u)  return JIT_HOST_R0 + g;
    if (g == 13u) return JIT_HOST_SP;
    return 32u;
}
static unsigned load_greg(jit_t *t, unsigned g, unsigned scratch) {
    unsigned h = host_of(g);
    if (h != 32u) return h;
    a64_ldr_uimm(&t->e, A64_SZ_W, scratch, JIT_HOST_CPU, OFF_R(g));
    return scratch;
}
static unsigned dest_greg(unsigned g) {
    unsigned h = host_of(g);
    return (h != 32u) ? h : JIT_HOST_S2;
}
static void commit_greg(jit_t *t, unsigned g, unsigned h) {
    if (host_of(g) == 32u)
        a64_str_uimm(&t->e, A64_SZ_W, h, JIT_HOST_CPU, OFF_R(g));
}

/* -------------------------------------------------- prologue / epilogue -- */

static void emit_prologue(jit_t *t) {
    a64_emit_t *e = &t->e;
    /* An emitted block is an ordinary AAPCS64 function: int (*)(arm_cpu_t *).
     * x19..x28 are callee-saved and hold guest state, so they are saved here
     * and restored in the epilogue; x29 is kept a valid frame pointer so
     * crash reports and lldb can walk through JIT frames (§8.4). */
    a64_stp_pre(e, A64_SZ_D, 29, 30, A64_SP, -JIT_FRAME_SIZE);
    a64_mov_sp (e, A64_X, 29, A64_SP);
    a64_stp(e, A64_SZ_D, 19, 20, A64_SP, JIT_FRAME_SAVED + 0);
    a64_stp(e, A64_SZ_D, 21, 22, A64_SP, JIT_FRAME_SAVED + 16);
    a64_stp(e, A64_SZ_D, 23, 24, A64_SP, JIT_FRAME_SAVED + 32);
    a64_stp(e, A64_SZ_D, 25, 26, A64_SP, JIT_FRAME_SAVED + 48);
    a64_stp(e, A64_SZ_D, 27, 28, A64_SP, JIT_FRAME_SAVED + 64);

    a64_mov_reg(e, A64_X, JIT_HOST_CPU, 0);
    a64_ldp(e, A64_SZ_W, 19, 20, JIT_HOST_CPU, OFF_R(0));
    a64_ldp(e, A64_SZ_W, 21, 22, JIT_HOST_CPU, OFF_R(2));
    a64_ldp(e, A64_SZ_W, 23, 24, JIT_HOST_CPU, OFF_R(4));
    a64_ldp(e, A64_SZ_W, 25, 26, JIT_HOST_CPU, OFF_R(6));
    a64_ldr_uimm(e, A64_SZ_W, 27, JIT_HOST_CPU, OFF_R(13));
    a64_ldr_uimm(e, A64_SZ_W, JIT_HOST_S0, JIT_HOST_CPU, OFF_CPSR);
    a64_msr_nzcv(e, JIT_HOST_S0);
}

/*
 * Every exit funnels here with w0 = reason, w1 = instructions retired and
 * w8 = the guest PC to resume at.
 */
static void emit_epilogue(jit_t *t) {
    a64_emit_t *e = &t->e;
    a64_str_uimm(e, A64_SZ_W, JIT_HOST_PCOUT, JIT_HOST_CPU, OFF_R(15));
    a64_stp(e, A64_SZ_W, 19, 20, JIT_HOST_CPU, OFF_R(0));
    a64_stp(e, A64_SZ_W, 21, 22, JIT_HOST_CPU, OFF_R(2));
    a64_stp(e, A64_SZ_W, 23, 24, JIT_HOST_CPU, OFF_R(4));
    a64_stp(e, A64_SZ_W, 25, 26, JIT_HOST_CPU, OFF_R(6));
    a64_str_uimm(e, A64_SZ_W, 27, JIT_HOST_CPU, OFF_R(13));

    /* Merge N/Z/C/V back into cpu->cpsr without disturbing I/F/T/Q/mode. */
    a64_mrs_nzcv(e, JIT_HOST_S0);
    a64_ldr_uimm(e, A64_SZ_W, JIT_HOST_S1, JIT_HOST_CPU, OFF_CPSR);
    a64_ubfx(e, A64_W, JIT_HOST_S0, JIT_HOST_S0, 28, 4);
    a64_bfi (e, A64_W, JIT_HOST_S1, JIT_HOST_S0, 28, 4);
    a64_str_uimm(e, A64_SZ_W, JIT_HOST_S1, JIT_HOST_CPU, OFF_CPSR);

    /* cycles += retired. The interpreter charges one per retired instruction
     * and the differential harness compares this field (§7.6). */
    a64_ldr_uimm(e, A64_SZ_D, JIT_HOST_S0, JIT_HOST_CPU, OFF_CYCLES);
    a64_add_reg (e, A64_X, JIT_HOST_S0, JIT_HOST_S0, 1, A64_LSL, 0);
    a64_str_uimm(e, A64_SZ_D, JIT_HOST_S0, JIT_HOST_CPU, OFF_CYCLES);

    a64_ldp(e, A64_SZ_D, 27, 28, A64_SP, JIT_FRAME_SAVED + 64);
    a64_ldp(e, A64_SZ_D, 25, 26, A64_SP, JIT_FRAME_SAVED + 48);
    a64_ldp(e, A64_SZ_D, 23, 24, A64_SP, JIT_FRAME_SAVED + 32);
    a64_ldp(e, A64_SZ_D, 21, 22, A64_SP, JIT_FRAME_SAVED + 16);
    a64_ldp(e, A64_SZ_D, 19, 20, A64_SP, JIT_FRAME_SAVED + 0);
    a64_ldp_post(e, A64_SZ_D, 29, 30, A64_SP, JIT_FRAME_SIZE);
    a64_ret(e, 30);
}

static void emit_exit(jit_t *t, uint32_t resume_pc, unsigned reason, unsigned retired) {
    a64_emit_t *e = &t->e;
    a64_mov_imm(e, A64_W, JIT_HOST_PCOUT, resume_pc);
    a64_movz(e, A64_W, 0, reason, 0);
    a64_movz(e, A64_W, 1, retired, 0);
    if (t->n_epi < sizeof t->epi / sizeof t->epi[0]) t->epi[t->n_epi++] = e->n;
    else t->e.bad = true;
    a64_b(e, 0);                       /* bound to the epilogue at the end */
}

/*
 * The same exit, for a branch whose target is only known at run time — every
 * Thumb BX/BLX, and the BL/BLX suffix pair, whose target is LR-relative.
 * `wpc` must already hold the resume address with bit 0 cleared; the T bit is
 * a separate, explicit update (emit_set_thumb_bit) because the two are
 * different pieces of architectural state and conflating them is precisely
 * how an interworking bug hides.
 */
static void emit_exit_reg(jit_t *t, unsigned wpc, unsigned reason, unsigned retired) {
    a64_emit_t *e = &t->e;
    a64_mov_reg(e, A64_W, JIT_HOST_PCOUT, wpc);
    a64_movz(e, A64_W, 0, reason, 0);
    a64_movz(e, A64_W, 1, retired, 0);
    if (t->n_epi < sizeof t->epi / sizeof t->epi[0]) t->epi[t->n_epi++] = e->n;
    else t->e.bad = true;
    a64_b(e, 0);
}

/* ------------------------------------------------------------ operand 2 -- */

typedef struct {
    bool        is_imm;
    uint32_t    imm;
    bool        carry_known;   /* the shifter carry-out is a translate-time constant */
    bool        carry_val;
    bool        carry_unaffected;
    unsigned    rm;
    a64_shift_t sh;
    unsigned    amt;
} op2_t;

static uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31u;
    return n ? ((v >> n) | (v << (32 - n))) : v;
}

/*
 * Decode a data-processing shifter operand, refusing every form whose exact
 * ARMv6 semantics are not reproducible with a single A64 operand (see the
 * DEFERRED list at the top of this file).
 */
static bool decode_op2(uint32_t insn, op2_t *o) {
    memset(o, 0, sizeof *o);
    if (insn & (1u << 25)) {                    /* rotated 8-bit immediate */
        unsigned rot = ((insn >> 8) & 0xfu) * 2u;
        o->is_imm = true;
        o->imm    = ror32(insn & 0xffu, rot);
        o->sh     = A64_LSL;
        if (rot == 0) o->carry_unaffected = true;
        else { o->carry_known = true; o->carry_val = (o->imm >> 31) & 1u; }
        return true;
    }
    if (insn & (1u << 4)) return false;         /* register-specified shift */
    o->rm  = insn & 0xfu;
    o->amt = (insn >> 7) & 0x1fu;
    if (o->rm == 15u) return false;             /* Rm == PC: §7.5, later */
    switch ((insn >> 5) & 3u) {
        case 0: o->sh = A64_LSL; break;                       /* LSL #0..31 */
        case 1: if (o->amt == 0) return false; o->sh = A64_LSR; break; /* #0 means #32 */
        case 2: if (o->amt == 0) return false; o->sh = A64_ASR; break; /* #0 means #32 */
        default:if (o->amt == 0) return false; o->sh = A64_ROR; break; /* #0 means RRX */
    }
    if (o->sh == A64_LSL && o->amt == 0) o->carry_unaffected = true;
    return true;
}

/* Materialise operand 2 into a register when an immediate form will not do. */
static unsigned op2_reg(jit_t *t, const op2_t *o, unsigned scratch) {
    if (o->is_imm) { a64_mov_imm(&t->e, A64_W, scratch, o->imm); return scratch; }
    return load_greg(t, o->rm, scratch);
}

/* Defined with the Thumb code below, which needs the general form. */
typedef enum { TF_C_PRESERVE = 0, TF_C_REG } tflag_c_t;
static void emit_nz_flags(jit_t *t, unsigned wres, tflag_c_t mode, unsigned wcarry);

/*
 * N and Z from the result, C from the shifter carry-out, V preserved — the
 * A32 logical-flag rule that A64's ANDS does not implement (§5.2(1)).
 */
static void emit_logic_flags(jit_t *t, unsigned wres, const op2_t *o) {
    a64_emit_t *e = &t->e;
    if (o->carry_unaffected) {
        /* C keeps its incoming value; V is preserved. Save both, recompute
         * N/Z, splice them back — the same sequence Thumb needs, so there is
         * one copy of it. */
        emit_nz_flags(t, wres, TF_C_PRESERVE, 0);
    } else {
        /* Rotated immediate: the carry-out is bit 31 of the rotated constant,
         * known here. Only V has to survive. */
        a64_mrs_nzcv(e, JIT_HOST_S0);
        a64_ubfx(e, A64_X, JIT_HOST_S0, JIT_HOST_S0, 28, 1);
        a64_ands_reg(e, A64_W, A64_ZR, wres, wres, A64_LSL, 0);
        a64_mrs_nzcv(e, JIT_HOST_S1);
        a64_bfi(e, A64_X, JIT_HOST_S1, JIT_HOST_S0, 28, 1);
        if (o->carry_val)
            a64_orr_imm(e, A64_X, JIT_HOST_S1, JIT_HOST_S1, (uint64_t)1 << 29);
        else
            a64_and_imm(e, A64_X, JIT_HOST_S1, JIT_HOST_S1, ~((uint64_t)1 << 29));
        a64_msr_nzcv(e, JIT_HOST_S1);
    }
}

/* ------------------------------------------------------ data processing -- */

/* Which opcodes take their C from the barrel shifter rather than the adder. */
static bool opcode_is_logical(unsigned op) {
    switch (op) {
        case 0x0: case 0x1: case 0x8: case 0x9:
        case 0xc: case 0xd: case 0xe: case 0xf: return true;
        default: return false;
    }
}
/*
 * Which opcodes need operand 2 delivered as a plain, already-shifted register.
 *
 * ADC/SBC/RSC because A64's ADC/SBC take no shifted operand at all; and RSB
 * (and RSC) because they are emitted with the operands *swapped*, which puts
 * operand 2 in the A64 Rn position — and A64 shifts Rm, not Rn. Emitting
 * `subs wd, wOp2, wRn, lsl #0` for `RSB Rd, Rn, Rm, LSL #4` would silently
 * drop the shift, so those forms are refused rather than approximated.
 */
static bool opcode_needs_plain_op2(unsigned op) {
    return op == 0x3 || op == 0x5 || op == 0x6 || op == 0x7;
}
/* Which opcodes write a result. */
static bool opcode_writes(unsigned op) { return op < 0x8 || op > 0xb; }

static bool dp_supported(uint32_t insn, const op2_t *o) {
    unsigned op = (insn >> 21) & 0xfu;
    bool     S  = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;

    /* Opcodes 8..b with S clear are the miscellaneous space (BX, MRS, MSR,
     * CLZ, saturating arithmetic) — never a comparison. */
    if (op >= 0x8u && op <= 0xbu && !S) return false;
    /* Rd == 15 is a branch, and with S set it is an exception return whose
     * alignment rule must exist in exactly one place (§7.4). Rn == 15 reads
     * pc + 8 (§7.5). Both are later work. */
    if (rd == 15u || rn == 15u) return false;
    /* A shifter carry-out that is not a translate-time constant, feeding a
     * logical S-setting instruction, is not attempted. */
    if (S && opcode_is_logical(op) && !o->carry_unaffected && !o->carry_known)
        return false;
    if (opcode_needs_plain_op2(op) && !o->is_imm && !(o->sh == A64_LSL && o->amt == 0))
        return false;
    /* A64 add/sub have no ROR. */
    if (!opcode_is_logical(op) && o->sh == A64_ROR && !o->is_imm) return false;
    return true;
}

static void emit_dp(jit_t *t, uint32_t insn, const op2_t *o) {
    a64_emit_t *e = &t->e;
    unsigned op = (insn >> 21) & 0xfu;
    bool     S  = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    unsigned wa = 0, wb = 0, wd;
    bool     imm12_ok = o->is_imm && o->imm <= 0xfffu;

    /* CMP/CMN discard their result, so it goes to WZR. TST/TEQ discard theirs
     * too, but the logical flag fixup has to read it back, so it needs a real
     * scratch register. */
    if (opcode_writes(op))        wd = dest_greg(rd);
    else if (op == 0x8 || op == 0x9) wd = JIT_HOST_S2;
    else                          wd = A64_ZR;

    switch (op) {
    case 0x0:  /* AND */
    case 0x1:  /* EOR */
    case 0xc:  /* ORR */
    case 0xe:  /* BIC */
    case 0x8:  /* TST */
    case 0x9:  /* TEQ */ {
        uint32_t mask;
        bool     used_imm = false;
        wa = load_greg(t, rn, JIT_HOST_S0);
        /* BIC Rd,Rn,#imm is AND Rd,Rn,#~imm; TST is AND to a scratch and
         * TEQ is EOR to a scratch. */
        mask = (op == 0xe) ? ~o->imm : o->imm;
        if (o->is_imm) {
            uint32_t enc;
            unsigned real = (op == 0x8) ? 0x0u : (op == 0x9 ? 0x1u : op);
            if (a64_bitmask_imm(A64_W, mask, &enc)) {
                used_imm = true;
                if (real == 0x0u || real == 0xeu) a64_and_imm(e, A64_W, wd, wa, mask);
                else if (real == 0x1u)            a64_eor_imm(e, A64_W, wd, wa, mask);
                else                              a64_orr_imm(e, A64_W, wd, wa, mask);
            }
        }
        if (!used_imm) {
            wb = op2_reg(t, o, JIT_HOST_S1);
            switch (op) {
                case 0x0: case 0x8: a64_and_reg(e, A64_W, wd, wa, wb, o->sh, o->amt); break;
                case 0x1: case 0x9: a64_eor_reg(e, A64_W, wd, wa, wb, o->sh, o->amt); break;
                case 0xc:           a64_orr_reg(e, A64_W, wd, wa, wb, o->sh, o->amt); break;
                default:            a64_bic_reg(e, A64_W, wd, wa, wb, o->sh, o->amt); break;
            }
        }
        if (S || op == 0x8 || op == 0x9) emit_logic_flags(t, wd, o);
        break;
    }
    case 0xd:  /* MOV */
    case 0xf:  /* MVN */
        if (o->is_imm) {
            a64_mov_imm(e, A64_W, wd, (op == 0xf) ? ~o->imm : o->imm);
        } else {
            wb = load_greg(t, o->rm, JIT_HOST_S1);
            if (op == 0xd) a64_orr_reg(e, A64_W, wd, A64_ZR, wb, o->sh, o->amt);
            else           a64_orn_reg(e, A64_W, wd, A64_ZR, wb, o->sh, o->amt);
        }
        if (S) emit_logic_flags(t, wd, o);
        break;

    case 0x2:  /* SUB */
    case 0xa:  /* CMP */
        wa = load_greg(t, rn, JIT_HOST_S0);
        if (imm12_ok) {
            if (S || op == 0xa) a64_subs_imm(e, A64_W, wd, wa, o->imm, false);
            else                a64_sub_imm (e, A64_W, wd, wa, o->imm, false);
        } else {
            wb = op2_reg(t, o, JIT_HOST_S1);
            if (S || op == 0xa) a64_subs_reg(e, A64_W, wd, wa, wb, o->sh, o->amt);
            else                a64_sub_reg (e, A64_W, wd, wa, wb, o->sh, o->amt);
        }
        break;

    case 0x4:  /* ADD */
    case 0xb:  /* CMN */
        wa = load_greg(t, rn, JIT_HOST_S0);
        if (imm12_ok) {
            if (S || op == 0xb) a64_adds_imm(e, A64_W, wd, wa, o->imm, false);
            else                a64_add_imm (e, A64_W, wd, wa, o->imm, false);
        } else {
            wb = op2_reg(t, o, JIT_HOST_S1);
            if (S || op == 0xb) a64_adds_reg(e, A64_W, wd, wa, wb, o->sh, o->amt);
            else                a64_add_reg (e, A64_W, wd, wa, wb, o->sh, o->amt);
        }
        break;

    case 0x3:  /* RSB: op2 - Rn, so swap the operands */
        wa = load_greg(t, rn, JIT_HOST_S0);
        wb = op2_reg(t, o, JIT_HOST_S1);
        if (S) a64_subs_reg(e, A64_W, wd, wb, wa, A64_LSL, 0);
        else   a64_sub_reg (e, A64_W, wd, wb, wa, A64_LSL, 0);
        break;

    case 0x5:  /* ADC */
        wa = load_greg(t, rn, JIT_HOST_S0);
        wb = op2_reg(t, o, JIT_HOST_S1);
        if (S) a64_adcs(e, A64_W, wd, wa, wb); else a64_adc(e, A64_W, wd, wa, wb);
        break;
    case 0x6:  /* SBC */
        wa = load_greg(t, rn, JIT_HOST_S0);
        wb = op2_reg(t, o, JIT_HOST_S1);
        if (S) a64_sbcs(e, A64_W, wd, wa, wb); else a64_sbc(e, A64_W, wd, wa, wb);
        break;
    case 0x7:  /* RSC: op2 - Rn - NOT(C) */
        wa = load_greg(t, rn, JIT_HOST_S0);
        wb = op2_reg(t, o, JIT_HOST_S1);
        if (S) a64_sbcs(e, A64_W, wd, wb, wa); else a64_sbc(e, A64_W, wd, wb, wa);
        break;
    default:
        t->e.bad = true;
        return;
    }

    if (opcode_writes(op)) commit_greg(t, rd, wd);
}

/* ------------------------------------------------- single data transfer -- */

static bool ldst_supported(uint32_t insn) {
    bool I = (insn >> 25) & 1u, P = (insn >> 24) & 1u, W = (insn >> 21) & 1u;
    unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    if (I) return false;                 /* register offset: later          */
    if (!P || W) return false;           /* writeback / post-index: later   */
    if (rn == 15u || rd == 15u) return false;  /* PC operands: §7.5         */
    return true;
}

/* Access width for the memory helpers, and how a loaded value is widened. */
typedef enum { JM_B = 0, JM_H = 1, JM_W = 2 } jmem_t;
typedef enum { JX_ZERO = 0, JX_SIGN } jext_t;

/*
 * cpu->r[15] must be exact at any faultable instruction: the interpreter
 * re-executes that one instruction on a fault and LR_abt is pc + 8 in both
 * instruction sets (arm_interp.c takes the Thumb data abort with pc + 8 too).
 */
static void emit_pc_for_fault(jit_t *t) {
    a64_mov_imm (&t->e, A64_W, JIT_HOST_PCOUT, t->pc);
    a64_str_uimm(&t->e, A64_SZ_W, JIT_HOST_PCOUT, JIT_HOST_CPU, OFF_R(15));
}

/*
 * Call a memory helper for an access whose guest address is already in w1 and,
 * for a store, whose value is already in w2; then commit a load's result into
 * guest register `rd`.
 *
 * Shared by the ARM and Thumb paths so there is one fault protocol, not two.
 */
static void emit_mem_call(jit_t *t, bool load, jmem_t sz, jext_t ext, unsigned rd) {
    a64_emit_t *e = &t->e;
    size_t    ok_site;
    uintptr_t helper;

    /* NZCV does not survive a C call (§4.4). */
    a64_mrs_nzcv(e, JIT_HOST_S0);
    a64_str_uimm(e, A64_SZ_D, JIT_HOST_S0, A64_SP, JIT_FRAME_SCRATCH);

    a64_mov_reg(e, A64_X, 0, JIT_HOST_CPU);
    {
        /* Taking the numeric value of a function pointer is not something ISO
         * C blesses; go through a union rather than a cast so -Wpedantic
         * builds stay clean. */
        union { uint64_t (*ld)(arm_cpu_t *, uint32_t);
                uint32_t (*st)(arm_cpu_t *, uint32_t, uint32_t);
                uintptr_t v; } u;
        if (load) u.ld = (sz == JM_B) ? jit_helper_load8
                       : (sz == JM_H) ? jit_mem_load16 : jit_mem_load32;
        else      u.st = (sz == JM_B) ? jit_helper_store8
                       : (sz == JM_H) ? jit_mem_store16 : jit_mem_store32;
        helper = u.v;
    }
    /* A BL cannot reach an arbitrary host address from the code arena, so the
     * helper address is materialised into IP0 and called indirectly. */
    a64_mov_imm(e, A64_X, JIT_HOST_HELPER, (uint64_t)helper);
    a64_blr(e, JIT_HOST_HELPER);

    a64_ldr_uimm(e, A64_SZ_D, JIT_HOST_S0, A64_SP, JIT_FRAME_SCRATCH);
    a64_msr_nzcv(e, JIT_HOST_S0);

    /* Fault? Loads report it in bit 32, stores in bits 31:0. Branch over the
     * exit stub on the (overwhelmingly common) success path. */
    ok_site = e->n;
    if (load) a64_tbz(e, 0, 32, 0);
    else      a64_cbz(e, A64_W, 0, 0);
    emit_exit(t, t->pc, JIT_EXIT_ABORT, t->index);
    a64_bind(e, ok_site);

    if (load) {
        unsigned wd = dest_greg(rd);
        /* The helper already zero-extends; only LDRSB/LDRSH need more. */
        if (ext == JX_SIGN && sz == JM_B)      a64_sbfx(e, A64_W, wd, 0, 0, 8);
        else if (ext == JX_SIGN && sz == JM_H) a64_sbfx(e, A64_W, wd, 0, 0, 16);
        else                                   a64_mov_reg(e, A64_W, wd, 0);
        commit_greg(t, rd, wd);
    }
}

static void emit_ldst(jit_t *t, uint32_t insn) {
    a64_emit_t *e = &t->e;
    bool     U  = (insn >> 23) & 1u, B = (insn >> 22) & 1u, L = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    uint32_t imm = insn & 0xfffu;
    unsigned wbase;

    emit_pc_for_fault(t);

    wbase = load_greg(t, rn, JIT_HOST_S0);
    if (U) a64_add_imm(e, A64_W, 1, wbase, imm, false);
    else   a64_sub_imm(e, A64_W, 1, wbase, imm, false);

    if (!L) {
        unsigned wval = load_greg(t, rd, JIT_HOST_S1);
        a64_mov_reg(e, A64_W, 2, wval);
    }
    emit_mem_call(t, L, B ? JM_B : JM_W, JX_ZERO, rd);
}

/* ------------------------------------------------------------- branches -- */

static void emit_branch(jit_t *t, uint32_t insn) {
    a64_emit_t *e = &t->e;
    int32_t  off    = (int32_t)(insn << 8) >> 6;      /* sign-extend 24 bits, <<2 */
    uint32_t target = (t->pc + 8u + (uint32_t)off) & ~3u;
    if (insn & (1u << 24)) {                          /* BL: LR = return address */
        a64_mov_imm(e, A64_W, JIT_HOST_S0, t->pc + 4u);
        a64_str_uimm(e, A64_SZ_W, JIT_HOST_S0, JIT_HOST_CPU, OFF_R(14));
    }
    emit_exit(t, target, JIT_EXIT_NEXT, t->index + 1u);
}

/* ================================================================= Thumb ==
 *
 * 68.95% of the instructions the real 3.1.3 kernel retires during init are
 * Thumb (docs/dynarec.md §1.3), so this is not an extension of the ARM
 * translator — it is the majority of the workload. The subset below was chosen
 * by measuring, not by guessing: every encoding class was histogrammed over
 * the first 20,000,000 retired instructions of the real kernelcache and the
 * classes are implemented in descending order of that share. What is declined
 * is declined because implementing it would mean writing a *second* copy of
 * semantics the interpreter already owns, and each decline is listed with the
 * share it costs at the bottom of this comment.
 *
 * ------------------------------- FLAGS ------------------------------------
 *
 * The single largest difference from the ARM translator: **almost every
 * Thumb-1 data-processing instruction sets N/Z/C/V unconditionally**, with no
 * S bit to opt out. There is no conditional-execution wrapper either — the
 * only conditional 16-bit instruction is B<cond> — so a Thumb block is
 * straight-line code with a flag update after nearly every instruction.
 *
 * The A32 rule the flags must follow is the interpreter's alu_logic_flags():
 * N and Z from the result, C from the barrel-shifter carry-out, V *preserved*.
 * A64's ANDS forces C = 0 and V = 0, so emit_nz_flags() splices them back.
 * Where the ARM translator could refuse a shifter carry-out it did not know at
 * translate time, Thumb cannot: LSLS/LSRS/ASRS by an immediate are 2.48% of
 * retired instructions and always set C. So the carry-out is computed
 * explicitly into a register (UBFX of the *source*, before the destination is
 * written, because Rd == Rs is the common case) and inserted with BFI.
 *
 * ---------------------- ARM/THUMB STATE TRANSITIONS -----------------------
 *
 * This is the highest-risk area in the file and it is handled by three rules.
 *
 * 1. A BLOCK NEVER SPANS A STATE CHANGE. Every instruction that can write the
 *    T bit or PC ends the block (§3.2 rules 1 and 2). The block key carries
 *    JIT_BLK_THUMB, so the successor is looked up — and, later, translated —
 *    in whatever state the exit left behind.
 *
 * 2. THE T BIT IS WRITTEN EXPLICITLY, NEVER INFERRED. emit_set_thumb_bit()
 *    inserts bit 0 of the branch target into CPSR bit 5 with a single BFI,
 *    which is exactly the interpreter's rule: it clears T when the target is
 *    even and leaves it alone when odd, and we start from T == 1, so
 *    `T := target[0]` is the same function. For BLX (immediate) the new state
 *    is a translate-time constant (always ARM), so WZR is inserted instead.
 *    The epilogue re-reads cpu->cpsr when it splices NZCV back, so a T update
 *    made in the body survives.
 *
 * 3. THE JIT CONTAINS NO EXCEPTION-RETURN LOGIC AT ALL (§7.4). SWI, POP {..,pc}
 *    and every hi-register form that writes PC are declined, so the alignment
 *    rule that once resumed a Thumb handler two bytes early — and unlocked a
 *    mutex at address 1 — still exists in exactly one place in this codebase.
 *
 * PC as an operand is free: Thumb reads r15 as pc + 4, word-aligned for the
 * PC-relative load and the PC form of ADD Rd,PC (§7.5), and both are
 * translate-time constants materialised with MOVZ/MOVK.
 *
 * ----------------------------- DECLINED -----------------------------------
 * Measured share of all retired instructions in the 20M-instruction run:
 *
 *   POP {..,pc}                     1.38%  writes PC and can switch state;
 *                                          it is an exception-return-shaped
 *                                          path and belongs to the interpreter
 *   PUSH / POP without pc           1.76%  base-restored abort model (§7.3):
 *   LDMIA / STMIA Rb!,{list}        1.24%  the interpreter buffers every word
 *                                          and commits only after all accesses
 *                                          succeed. Reimplementing that here
 *                                          would be a second implementation of
 *                                          the one thing §7.3 says must have
 *                                          only one. Needs a state helper
 *                                          (full register sync + a call into
 *                                          the interpreter), which is J5.
 *   ALU shift-by-register           0.06%  A32 shifts by Rs[7:0] with distinct
 *                                          answers at 0, 32 and >32; A64's
 *                                          LSLV is modulo 32 (§5.2(2)).
 *   hi-register form writing PC     0.01%  the interpreter's ADD pc,Rm adds
 *                                          r[15] rather than pc + 4; that is a
 *                                          divergence from hardware and this
 *                                          file will not bake a second copy of
 *                                          it in (§5.4).
 *   SWI, BKPT, CPS, SETEND, REV*    0.00%  not retired at all in the measured
 *                                          run; mode changes and the undefined
 *                                          space belong to the interpreter.
 *
 * IT BLOCKS AND THUMB-2 ARE DECLINED EXPLICITLY, not by omission. `IT` is
 * ARMv7 (0xbf__), it does not exist on this ARMv6 core, and the interpreter
 * does not implement it — so it lands in the 0xb__ default here and ends the
 * block, which is the only safe answer: a translator that quietly ignored an
 * `IT` would execute up to four following instructions unconditionally. The
 * 32-bit Thumb-2 encodings occupy 0xe800-0xffff, which on ARMv6 *is* the
 * BL/BLX prefix-suffix space, and that is exactly how both this file and
 * thumb_step() decode it. When ARMv7 arrives (§12) this decision has to be
 * revisited in both engines at once, not just here.
 */

/* 3-bit register field at bit `n`. */
static unsigned tb3(uint16_t insn, unsigned n) { return (unsigned)((insn >> n) & 7u); }

/* The Thumb forms this file translates. TT_NONE means "the interpreter's". */
typedef enum {
    TT_NONE = 0,
    TT_SHIFT_IMM,     /* LSL/LSR/ASR Rd,Rs,#imm5            000 op imm5 s d  */
    TT_ADDSUB,        /* ADD/SUB Rd,Rs,Rn|#imm3             00011 I op n s d */
    TT_IMM8,          /* MOV/CMP/ADD/SUB Rd,#imm8           001 op d imm8    */
    TT_ALU,           /* the 010000 ALU group                                */
    TT_HIREG,         /* hi-register ADD/CMP/MOV, Rd != 15  010001 op h1h2   */
    TT_BX,            /* BX / BLX Rm                        01000111 L h2 m  */
    TT_LDR_PC,        /* LDR Rd,[PC,#imm8*4]                01001 d imm8     */
    TT_LDST_REG,      /* the 0101 register-offset group                      */
    TT_LDST_IMM,      /* word/byte imm5 (0110/0111), halfword imm5 (1000)    */
    TT_LDST_SP,       /* LDR/STR Rd,[SP,#imm8*4]            1001 L d imm8    */
    TT_ADD_PCSP,      /* ADD Rd,PC|SP,#imm8*4               1010 S d imm8    */
    TT_ADJ_SP,        /* ADD/SUB SP,#imm7*4                 10110000 S imm7  */
    TT_EXTEND,        /* SXTH/SXTB/UXTH/UXTB                10110010 op s d  */
    TT_BCOND,         /* B<cond> #imm8                      1101 cond imm8   */
    TT_B,             /* B #imm11                           11100 imm11      */
    TT_BLX_SUFFIX,    /* BLX suffix, returns to ARM         11101 imm11      */
    TT_BL_PREFIX,     /* BL/BLX prefix: LR = pc+4+imm       11110 imm11      */
    TT_BL_SUFFIX      /* BL suffix, stays Thumb             11111 imm11      */
} thumb_form_t;

/* Which deny class (docs/dynarec.md §2) a form belongs to. */
static uint32_t thumb_deny_class(thumb_form_t f) {
    switch (f) {
        case TT_LDR_PC: case TT_LDST_REG: case TT_LDST_IMM: case TT_LDST_SP:
            return JIT_DENY_LDST;
        case TT_BX: case TT_BCOND: case TT_B:
        case TT_BLX_SUFFIX: case TT_BL_PREFIX: case TT_BL_SUFFIX:
            return JIT_DENY_BRANCH;
        default:
            return JIT_DENY_DP;
    }
}
/* Which forms write PC and therefore end the block (§3.2 rule 1). */
static bool thumb_form_ends_block(thumb_form_t f) {
    return f == TT_BX || f == TT_BCOND || f == TT_B
        || f == TT_BLX_SUFFIX || f == TT_BL_SUFFIX;
}

/*
 * Classify one 16-bit Thumb instruction, in the same order as thumb_step() so
 * the two cannot disagree about what an encoding *is*. Decide here, emit
 * later: a form that is not translated must leave nothing in the buffer.
 */
static thumb_form_t thumb_classify(uint16_t insn) {
    switch (insn >> 12) {
    case 0x0: case 0x1:
        return ((insn & 0xf800u) == 0x1800u) ? TT_ADDSUB : TT_SHIFT_IMM;
    case 0x2: case 0x3:
        return TT_IMM8;
    case 0x4:
        if ((insn & 0xfc00u) == 0x4000u) {
            unsigned op = (insn >> 6) & 0xfu;
            /* LSL/LSR/ASR/ROR by a register: A32 distinguishes shift amounts
             * 0, 32 and > 32; A64's LSLV is modulo 32 (§5.2(2)). 0.06%. */
            if (op == 0x2u || op == 0x3u || op == 0x4u || op == 0x7u) return TT_NONE;
            return TT_ALU;
        }
        if ((insn & 0xfc00u) == 0x4400u) {
            unsigned op = (insn >> 8) & 3u;
            unsigned rd = tb3(insn, 0) | ((insn >> 4) & 8u);
            if (op == 3u) return TT_BX;
            /* Rd == 15 writes PC; see the DECLINED list. 0.01%. */
            return (rd == 15u) ? TT_NONE : TT_HIREG;
        }
        return TT_LDR_PC;
    case 0x5: return TT_LDST_REG;
    case 0x6: case 0x7: case 0x8: return TT_LDST_IMM;
    case 0x9: return TT_LDST_SP;
    case 0xa: return TT_ADD_PCSP;
    case 0xb:
        if ((insn & 0xff00u) == 0xb000u) return TT_ADJ_SP;
        if ((insn & 0xff00u) == 0xb200u) return TT_EXTEND;
        return TT_NONE;                       /* PUSH/POP, REV, CPS, SETEND  */
    case 0xc: return TT_NONE;                 /* LDMIA/STMIA (§7.3)          */
    case 0xd: {
        unsigned cond = (insn >> 8) & 0xfu;
        /* 0xf is SWI (an exception entry) and 0xe is permanently undefined. */
        if (cond >= 0xeu) return TT_NONE;
        return TT_BCOND;
    }
    case 0xe: return (insn & 0x0800u) ? TT_BLX_SUFFIX : TT_B;
    default:  return (insn & 0x0800u) ? TT_BL_SUFFIX  : TT_BL_PREFIX;
    }
}

/* ------------------------------------------------------- Thumb flag rule -- */

/*
 * TF_C_PRESERVE — C keeps its incoming value (no shifter is involved).
 * TF_C_REG      — C is bit 0 of a register the caller has already computed.
 *
 * N and Z from `wres`, V preserved, C either preserved or taken from bit 0 of
 * `wcarry`. This is alu_logic_flags() (arm_interp.c) expressed in A64, and it
 * is emitted after nearly every Thumb data-processing instruction.
 *
 *   mrs  x9,  nzcv              ; the incoming flags
 *   ubfx x9,  x9, #28, #2       ; save V (and C, when C is preserved)
 *   ands wzr, wres, wres        ; N,Z from the result — A64 also zeroes C,V
 *   mrs  x10, nzcv
 *   bfi  x10, x9, #28, #2       ; put them back
 *   bfi  x10, wcarry, #29, #1   ; TF_C_REG only
 *   msr  nzcv, x10
 *
 * `wcarry` must be a register the sequence does not itself use, which is why
 * JIT_HOST_S3 exists: S0 and S1 are both live inside this sequence.
 */
static void emit_nz_flags(jit_t *t, unsigned wres, tflag_c_t mode, unsigned wcarry) {
    a64_emit_t *e = &t->e;
    unsigned width = (mode == TF_C_PRESERVE) ? 2u : 1u;
    a64_mrs_nzcv(e, JIT_HOST_S0);
    a64_ubfx(e, A64_X, JIT_HOST_S0, JIT_HOST_S0, 28, width);
    a64_ands_reg(e, A64_W, A64_ZR, wres, wres, A64_LSL, 0);
    a64_mrs_nzcv(e, JIT_HOST_S1);
    a64_bfi(e, A64_X, JIT_HOST_S1, JIT_HOST_S0, 28, width);
    if (mode == TF_C_REG) a64_bfi(e, A64_X, JIT_HOST_S1, wcarry, 29, 1);
    a64_msr_nzcv(e, JIT_HOST_S1);
}

/*
 * cpu->cpsr.T := bit 0 of `wsrc` (pass A64_ZR for "definitely ARM").
 *
 *   ldr w10, [x28, #64]
 *   bfi w10, wsrc, #5, #1
 *   str w10, [x28, #64]
 *
 * Uses S1 rather than S0 so a caller can keep the branch target in S0.
 */
static void emit_set_thumb_bit(jit_t *t, unsigned wsrc) {
    a64_emit_t *e = &t->e;
    a64_ldr_uimm(e, A64_SZ_W, JIT_HOST_S1, JIT_HOST_CPU, OFF_CPSR);
    a64_bfi(e, A64_W, JIT_HOST_S1, wsrc, 5, 1);
    a64_str_uimm(e, A64_SZ_W, JIT_HOST_S1, JIT_HOST_CPU, OFF_CPSR);
}

/* LR = (pc + 2) | 1 — the Thumb return address, with the interworking bit. */
static void emit_thumb_link(jit_t *t) {
    a64_emit_t *e = &t->e;
    a64_mov_imm (e, A64_W, JIT_HOST_S1, (t->pc + 2u) | 1u);
    a64_str_uimm(e, A64_SZ_W, JIT_HOST_S1, JIT_HOST_CPU, OFF_R(14));
}

/* ------------------------------------------------------- Thumb emission -- */

/* LSL/LSR/ASR Rd,Rs,#imm5 — 000 op(2) imm5 Rs Rd. Always sets N,Z,C; V kept. */
static void emit_thumb_shift(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    unsigned rd = tb3(insn, 0), rs = tb3(insn, 3);
    unsigned amt = (insn >> 6) & 0x1fu, type = (insn >> 11) & 3u;
    unsigned wsrc = load_greg(t, rs, JIT_HOST_S0);
    unsigned wd   = dest_greg(rd);

    if (type == 0u && amt == 0u) {
        /* LSL #0 is a plain move; barrel_shift() leaves the carry alone. */
        a64_mov_reg(e, A64_W, wd, wsrc);
        emit_nz_flags(t, wd, TF_C_PRESERVE, 0);
        return;
    }
    /* The carry-out is read from the SOURCE before the destination is written,
     * because Rd == Rs is the common case and would otherwise destroy it. */
    switch (type) {
    case 0:  /* LSL #1..31: carry = val[32 - amt] */
        a64_ubfx(e, A64_W, JIT_HOST_S3, wsrc, 32u - amt, 1);
        a64_lsl_imm(e, A64_W, wd, wsrc, amt);
        break;
    case 1:  /* LSR: #0 encodes #32, giving 0 with carry = val[31] */
        if (amt == 0u) {
            a64_ubfx(e, A64_W, JIT_HOST_S3, wsrc, 31, 1);
            a64_mov_reg(e, A64_W, wd, A64_ZR);
        } else {
            a64_ubfx(e, A64_W, JIT_HOST_S3, wsrc, amt - 1u, 1);
            a64_lsr_imm(e, A64_W, wd, wsrc, amt);
        }
        break;
    default: /* ASR: #0 encodes #32, giving the sign bit smeared, carry val[31] */
        if (amt == 0u) {
            a64_ubfx(e, A64_W, JIT_HOST_S3, wsrc, 31, 1);
            a64_asr_imm(e, A64_W, wd, wsrc, 31);
        } else {
            a64_ubfx(e, A64_W, JIT_HOST_S3, wsrc, amt - 1u, 1);
            a64_asr_imm(e, A64_W, wd, wsrc, amt);
        }
        break;
    }
    emit_nz_flags(t, wd, TF_C_REG, JIT_HOST_S3);
}

/* ADD/SUB Rd,Rs,Rn|#imm3 — 00011 I op Rn/imm3 Rs Rd. Flags map exactly. */
static void emit_thumb_addsub(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    unsigned rd = tb3(insn, 0), rs = tb3(insn, 3);
    unsigned wd = dest_greg(rd), wsrc = load_greg(t, rs, JIT_HOST_S0);
    bool     sub = (insn >> 9) & 1u;
    if (insn & (1u << 10)) {                       /* 3-bit immediate */
        uint32_t imm = tb3(insn, 6);
        if (sub) a64_subs_imm(e, A64_W, wd, wsrc, imm, false);
        else     a64_adds_imm(e, A64_W, wd, wsrc, imm, false);
    } else {
        unsigned wm = load_greg(t, tb3(insn, 6), JIT_HOST_S1);
        if (sub) a64_subs_reg(e, A64_W, wd, wsrc, wm, A64_LSL, 0);
        else     a64_adds_reg(e, A64_W, wd, wsrc, wm, A64_LSL, 0);
    }
}

/* MOV/CMP/ADD/SUB Rd,#imm8 — 001 op(2) Rd imm8. */
static void emit_thumb_imm8(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    unsigned rd  = (insn >> 8) & 7u, wd = dest_greg(rd);
    uint32_t imm = insn & 0xffu;
    switch ((insn >> 11) & 3u) {
    case 0:  /* MOV: N,Z from the immediate; C and V untouched. */
        a64_mov_imm(e, A64_W, wd, imm);
        emit_nz_flags(t, wd, TF_C_PRESERVE, 0);
        break;
    case 1:  a64_subs_imm(e, A64_W, A64_ZR, wd, imm, false); break;  /* CMP */
    case 2:  a64_adds_imm(e, A64_W, wd, wd, imm, false);     break;  /* ADD */
    default: a64_subs_imm(e, A64_W, wd, wd, imm, false);     break;  /* SUB */
    }
}

/*
 * The 010000 ALU group: op(4) Rs Rd, operating on Rd and Rs only. The four
 * register-shift forms were refused by thumb_classify(); the rest are here.
 */
static void emit_thumb_alu(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    unsigned op = (insn >> 6) & 0xfu;
    unsigned rd = tb3(insn, 0), rs = tb3(insn, 3);
    unsigned wa = load_greg(t, rd, JIT_HOST_S0);
    unsigned wb = load_greg(t, rs, JIT_HOST_S1);
    unsigned wd = dest_greg(rd);

    switch (op) {
    case 0x0: a64_and_reg(e, A64_W, wd, wa, wb, A64_LSL, 0);            /* AND */
              emit_nz_flags(t, wd, TF_C_PRESERVE, 0); break;
    case 0x1: a64_eor_reg(e, A64_W, wd, wa, wb, A64_LSL, 0);            /* EOR */
              emit_nz_flags(t, wd, TF_C_PRESERVE, 0); break;
    case 0x5: a64_adcs(e, A64_W, wd, wa, wb); break;                    /* ADC */
    case 0x6: a64_sbcs(e, A64_W, wd, wa, wb); break;                    /* SBC */
    case 0x8: /* TST: the result is discarded, but the fixup must read it. */
              a64_and_reg(e, A64_W, JIT_HOST_S2, wa, wb, A64_LSL, 0);
              emit_nz_flags(t, JIT_HOST_S2, TF_C_PRESERVE, 0); break;
    case 0x9: a64_subs_reg(e, A64_W, wd, A64_ZR, wb, A64_LSL, 0); break;/* NEG */
    case 0xa: a64_subs_reg(e, A64_W, A64_ZR, wa, wb, A64_LSL, 0); break;/* CMP */
    case 0xb: a64_adds_reg(e, A64_W, A64_ZR, wa, wb, A64_LSL, 0); break;/* CMN */
    case 0xc: a64_orr_reg(e, A64_W, wd, wa, wb, A64_LSL, 0);            /* ORR */
              emit_nz_flags(t, wd, TF_C_PRESERVE, 0); break;
    case 0xd: /* MUL sets N and Z only; C is left alone on ARMv6 (§5.4) and
               * V is preserved, which is what TF_C_PRESERVE does. */
              a64_mul(e, A64_W, wd, wa, wb);
              emit_nz_flags(t, wd, TF_C_PRESERVE, 0); break;
    case 0xe: a64_bic_reg(e, A64_W, wd, wa, wb, A64_LSL, 0);            /* BIC */
              emit_nz_flags(t, wd, TF_C_PRESERVE, 0); break;
    case 0xf: a64_mvn_reg(e, A64_W, wd, wb);                            /* MVN */
              emit_nz_flags(t, wd, TF_C_PRESERVE, 0); break;
    default:  t->e.bad = true; return;
    }
    /* CMP/CMN/TST write no register. */
    if (op != 0x8u && op != 0xau && op != 0xbu) commit_greg(t, rd, wd);
}

/* Hi-register ADD/CMP/MOV — 010001 op(2) H1 H2 Rs Rd, with Rd != 15. */
static void emit_thumb_hireg(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    unsigned rd = tb3(insn, 0) | ((insn >> 4) & 8u);
    unsigned rs = tb3(insn, 3) | ((insn >> 3) & 8u);
    unsigned op = (insn >> 8) & 3u;
    unsigned wb;

    if (rs == 15u) {   /* reading r15 in Thumb yields pc + 4 — a constant */
        wb = JIT_HOST_S1;
        a64_mov_imm(e, A64_W, wb, t->pc + 4u);
    } else {
        wb = load_greg(t, rs, JIT_HOST_S1);
    }
    if (op == 1u) {                                   /* CMP: the only one
                                                       * that touches flags */
        unsigned wa = load_greg(t, rd, JIT_HOST_S0);
        a64_subs_reg(e, A64_W, A64_ZR, wa, wb, A64_LSL, 0);
        return;
    }
    {
        unsigned wd = dest_greg(rd);
        if (op == 0u) {                               /* ADD, no flags */
            unsigned wa = load_greg(t, rd, JIT_HOST_S0);
            a64_add_reg(e, A64_W, wd, wa, wb, A64_LSL, 0);
        } else {                                      /* MOV, no flags */
            a64_mov_reg(e, A64_W, wd, wb);
        }
        commit_greg(t, rd, wd);
    }
}

/*
 * BX / BLX Rm — 01000111 L H2 Rm 000. The interworking instruction, and the
 * one place a Thumb block's successor state is decided at run time.
 */
static void emit_thumb_bx(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    unsigned rs = tb3(insn, 3) | ((insn >> 3) & 8u);
    unsigned wsv;

    if (rs == 15u) { wsv = JIT_HOST_S0; a64_mov_imm(e, A64_W, wsv, t->pc + 4u); }
    else           { wsv = load_greg(t, rs, JIT_HOST_S0); }

    if (insn & (1u << 7)) emit_thumb_link(t);        /* BLX: LR = (pc+2)|1 */
    emit_set_thumb_bit(t, wsv);                      /* T := target[0]     */
    a64_and_imm(e, A64_W, JIT_HOST_S0, wsv, 0xfffffffeu);
    emit_exit_reg(t, JIT_HOST_S0, JIT_EXIT_NEXT, t->index + 1u);
}

/* LDR Rd,[PC,#imm8*4] — 01001 Rd imm8. The address is a constant (§7.5). */
static void emit_thumb_ldr_pc(jit_t *t, uint16_t insn) {
    unsigned rd   = (insn >> 8) & 7u;
    uint32_t addr = ((t->pc + 4u) & ~3u) + ((uint32_t)(insn & 0xffu) << 2);
    emit_pc_for_fault(t);
    a64_mov_imm(&t->e, A64_W, 1, addr);
    emit_mem_call(t, true, JM_W, JX_ZERO, rd);
}

/*
 * The 0101 register-offset group. Bit 9 selects between the plain
 * word/byte forms and the halfword/sign-extended ones, exactly as
 * thumb_step() does; getting that split wrong turns a STRH into a LDR.
 */
static void emit_thumb_ldst_reg(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    unsigned rd = tb3(insn, 0), rb = tb3(insn, 3), ro = tb3(insn, 6);
    bool     load;
    jmem_t   sz;
    jext_t   ext = JX_ZERO;

    if (insn & (1u << 9)) {
        switch ((insn >> 10) & 3u) {
            case 0:  load = false; sz = JM_H; break;                  /* STRH  */
            case 1:  load = true;  sz = JM_B; ext = JX_SIGN; break;   /* LDRSB */
            case 2:  load = true;  sz = JM_H; break;                  /* LDRH  */
            default: load = true;  sz = JM_H; ext = JX_SIGN; break;   /* LDRSH */
        }
    } else {
        load = (insn >> 11) & 1u;
        sz   = ((insn >> 10) & 1u) ? JM_B : JM_W;
    }

    emit_pc_for_fault(t);
    {
        unsigned wb = load_greg(t, rb, JIT_HOST_S0);
        unsigned wo = load_greg(t, ro, JIT_HOST_S1);
        a64_add_reg(e, A64_W, 1, wb, wo, A64_LSL, 0);
    }
    if (!load) {
        unsigned wv = load_greg(t, rd, JIT_HOST_S1);
        a64_mov_reg(e, A64_W, 2, wv);
    }
    emit_mem_call(t, load, sz, ext, rd);
}

/* Immediate-offset load/store: word/byte (0110/0111) and halfword (1000). */
static void emit_thumb_ldst_imm(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    unsigned rd = tb3(insn, 0), rb = tb3(insn, 3);
    unsigned off = (insn >> 6) & 0x1fu;
    bool     load;
    jmem_t   sz;
    uint32_t scaled;

    if ((insn >> 12) == 0x8u) {                       /* halfword, imm5 * 2 */
        load = (insn >> 11) & 1u; sz = JM_H; scaled = off << 1;
    } else if ((insn >> 12) & 1u) {                   /* byte, imm5         */
        load = (insn >> 11) & 1u; sz = JM_B; scaled = off;
    } else {                                          /* word, imm5 * 4     */
        load = (insn >> 11) & 1u; sz = JM_W; scaled = off << 2;
    }

    emit_pc_for_fault(t);
    {
        unsigned wb = load_greg(t, rb, JIT_HOST_S0);
        a64_add_imm(e, A64_W, 1, wb, scaled, false);
    }
    if (!load) {
        unsigned wv = load_greg(t, rd, JIT_HOST_S1);
        a64_mov_reg(e, A64_W, 2, wv);
    }
    emit_mem_call(t, load, sz, JX_ZERO, rd);
}

/* LDR/STR Rd,[SP,#imm8*4] — 1001 L Rd imm8. SP is pinned in x27. */
static void emit_thumb_ldst_sp(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    unsigned rd = (insn >> 8) & 7u;
    bool     load = (insn >> 11) & 1u;
    emit_pc_for_fault(t);
    a64_add_imm(e, A64_W, 1, JIT_HOST_SP, (uint32_t)(insn & 0xffu) << 2, false);
    if (!load) {
        unsigned wv = load_greg(t, rd, JIT_HOST_S1);
        a64_mov_reg(e, A64_W, 2, wv);
    }
    emit_mem_call(t, load, JM_W, JX_ZERO, rd);
}

/* ADD Rd,PC|SP,#imm8*4 — 1010 S Rd imm8. No flags. */
static void emit_thumb_add_pcsp(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    unsigned rd  = (insn >> 8) & 7u, wd = dest_greg(rd);
    uint32_t imm = (uint32_t)(insn & 0xffu) << 2;
    if (insn & (1u << 11)) a64_add_imm(e, A64_W, wd, JIT_HOST_SP, imm, false);
    else                   a64_mov_imm(e, A64_W, wd, ((t->pc + 4u) & ~3u) + imm);
}

/* ADD/SUB SP,#imm7*4 — 10110000 S imm7. No flags. */
static void emit_thumb_adj_sp(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    uint32_t imm = (uint32_t)(insn & 0x7fu) << 2;
    if (insn & (1u << 7)) a64_sub_imm(e, A64_W, JIT_HOST_SP, JIT_HOST_SP, imm, false);
    else                  a64_add_imm(e, A64_W, JIT_HOST_SP, JIT_HOST_SP, imm, false);
}

/* SXTH/SXTB/UXTH/UXTB — 10110010 op(2) Rs Rd. No flags. */
static void emit_thumb_extend(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    unsigned rd = tb3(insn, 0), wd = dest_greg(rd);
    unsigned ws = load_greg(t, tb3(insn, 3), JIT_HOST_S0);
    switch ((insn >> 6) & 3u) {
        case 0:  a64_sbfx(e, A64_W, wd, ws, 0, 16); break;   /* SXTH */
        case 1:  a64_sbfx(e, A64_W, wd, ws, 0, 8);  break;   /* SXTB */
        case 2:  a64_ubfx(e, A64_W, wd, ws, 0, 16); break;   /* UXTH */
        default: a64_ubfx(e, A64_W, wd, ws, 0, 8);  break;   /* UXTB */
    }
}

/*
 * B<cond> — 1101 cond imm8, both edges static. Two exits, exactly as the ARM
 * conditional branch has, so chaining can later patch them independently.
 */
static void emit_thumb_bcond(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    unsigned cond = (insn >> 8) & 0xfu;
    uint32_t target = t->pc + 4u
                    + ((uint32_t)(int32_t)(int8_t)(insn & 0xffu) << 1);
    size_t skip = e->n;
    a64_bcond(e, a64_invert_cond((a64_cond_t)cond), 0);
    emit_exit(t, target, JIT_EXIT_NEXT, t->index + 1u);
    a64_bind(e, skip);
    emit_exit(t, t->pc + 2u, JIT_EXIT_NEXT, t->index + 1u);
}

/* B #imm11 — 11100 imm11, sign-extended and doubled from pc + 4. */
static void emit_thumb_b(jit_t *t, uint16_t insn) {
    int32_t off = (int32_t)((uint32_t)(insn & 0x7ffu) << 21) >> 20;
    emit_exit(t, t->pc + 4u + (uint32_t)off, JIT_EXIT_NEXT, t->index + 1u);
}

/*
 * The second half of a BL or BLX pair. Both take their target from LR, which
 * the prefix halfword loaded, so the target is a run-time value even though
 * the *state* change is a translate-time constant:
 *
 *   BLX suffix (11101): target = (LR + imm11*2) & ~3, and execution returns
 *                       to ARM — hence WZR into the T bit.
 *   BL  suffix (11111): target = (LR + imm11*2) & ~1, still Thumb, so T is
 *                       not touched at all.
 *
 * LR is read before it is overwritten with the return address; doing it the
 * other way round branches to the instruction after the call, forever.
 */
static void emit_thumb_bl_suffix(jit_t *t, uint16_t insn, bool to_arm) {
    a64_emit_t *e = &t->e;
    uint32_t imm = (uint32_t)(insn & 0x7ffu) << 1;
    a64_ldr_uimm(e, A64_SZ_W, JIT_HOST_S0, JIT_HOST_CPU, OFF_R(14));
    a64_add_imm (e, A64_W, JIT_HOST_S0, JIT_HOST_S0, imm, false);
    a64_and_imm (e, A64_W, JIT_HOST_S0, JIT_HOST_S0,
                 to_arm ? 0xfffffffcu : 0xfffffffeu);
    emit_thumb_link(t);
    if (to_arm) emit_set_thumb_bit(t, A64_ZR);
    emit_exit_reg(t, JIT_HOST_S0, JIT_EXIT_NEXT, t->index + 1u);
}

/* The first half: LR = pc + 4 + sign_extend(imm11) << 12. A constant. */
static void emit_thumb_bl_prefix(jit_t *t, uint16_t insn) {
    a64_emit_t *e = &t->e;
    int32_t off = (int32_t)((uint32_t)(insn & 0x7ffu) << 21) >> 9;
    a64_mov_imm (e, A64_W, JIT_HOST_S0, t->pc + 4u + (uint32_t)off);
    a64_str_uimm(e, A64_SZ_W, JIT_HOST_S0, JIT_HOST_CPU, OFF_R(14));
}

/* ----------------------------------------------------------- dispatcher -- */

typedef enum { TR_FALLBACK = 0, TR_CONTINUE, TR_ENDS_BLOCK } tr_result_t;

static tr_result_t translate_thumb_one(jit_t *t, uint16_t insn) {
    thumb_form_t f = thumb_classify(insn);
    if (f == TT_NONE) return TR_FALLBACK;
    if (g_deny & thumb_deny_class(f)) return TR_FALLBACK;

    switch (f) {
        case TT_SHIFT_IMM:  emit_thumb_shift(t, insn);          break;
        case TT_ADDSUB:     emit_thumb_addsub(t, insn);         break;
        case TT_IMM8:       emit_thumb_imm8(t, insn);           break;
        case TT_ALU:        emit_thumb_alu(t, insn);            break;
        case TT_HIREG:      emit_thumb_hireg(t, insn);          break;
        case TT_BX:         emit_thumb_bx(t, insn);             break;
        case TT_LDR_PC:     emit_thumb_ldr_pc(t, insn);         break;
        case TT_LDST_REG:   emit_thumb_ldst_reg(t, insn);       break;
        case TT_LDST_IMM:   emit_thumb_ldst_imm(t, insn);       break;
        case TT_LDST_SP:    emit_thumb_ldst_sp(t, insn);        break;
        case TT_ADD_PCSP:   emit_thumb_add_pcsp(t, insn);       break;
        case TT_ADJ_SP:     emit_thumb_adj_sp(t, insn);         break;
        case TT_EXTEND:     emit_thumb_extend(t, insn);         break;
        case TT_BCOND:      emit_thumb_bcond(t, insn);          break;
        case TT_B:          emit_thumb_b(t, insn);              break;
        case TT_BLX_SUFFIX: emit_thumb_bl_suffix(t, insn, true);  break;
        case TT_BL_SUFFIX:  emit_thumb_bl_suffix(t, insn, false); break;
        case TT_BL_PREFIX:  emit_thumb_bl_prefix(t, insn);      break;
        default:            t->e.bad = true;                    break;
    }
    return thumb_form_ends_block(f) ? TR_ENDS_BLOCK : TR_CONTINUE;
}

/*
 * Classify one ARM instruction, in the same order as arm_step()'s decode so
 * the two cannot disagree about what an encoding *is*. Anything not in the
 * (deliberately small) native set returns TR_FALLBACK and is left entirely to
 * the interpreter.
 */
static tr_result_t translate_one(jit_t *t, uint32_t insn) {
    a64_emit_t *e = &t->e;
    unsigned cond = insn >> 28;
    size_t   skip_site = 0;
    bool     conditional;
    tr_result_t result;
    op2_t    o;

    if (cond == 0xfu) return TR_FALLBACK;   /* unconditional space: PLD, CPS, ... */
    conditional = (cond != 0xeu);

    /* Decide first, emit second: the fallback decision must never leave half
     * an instruction in the buffer. */
    if ((insn & 0x0e000000u) == 0x0a000000u) {           /* B / BL */
        if (g_deny & JIT_DENY_BRANCH) return TR_FALLBACK;
        result = TR_ENDS_BLOCK;
    } else if ((insn & 0x0c000000u) == 0x04000000u) {    /* single data transfer */
        if ((insn & 0x0e000000u) == 0x06000000u) return TR_FALLBACK;  /* media space */
        if (g_deny & JIT_DENY_LDST) return TR_FALLBACK;
        if (!ldst_supported(insn)) return TR_FALLBACK;
        result = TR_CONTINUE;
    } else if ((insn & 0x0c000000u) == 0x00000000u) {    /* data processing */
        /*
         * With bit 25 clear, bits 7 and 4 both set means multiply, exclusives,
         * SWP or the extra load/store family — not data processing. The bit-25
         * guard is essential and is the same one arm_step() carries: an
         * immediate data-processing instruction sets bit 25 and its imm8 may
         * itself have bits 7 and 4 set (MOV r0,#0x90), so it must not be
         * diverted here.
         */
        if (!(insn & (1u << 25)) && (insn & 0x00000090u) == 0x00000090u)
            return TR_FALLBACK;
        if (g_deny & JIT_DENY_DP) return TR_FALLBACK;
        if (!decode_op2(insn, &o)) return TR_FALLBACK;
        if (!dp_supported(insn, &o)) return TR_FALLBACK;
        result = TR_CONTINUE;
    } else {
        return TR_FALLBACK;
    }

    if (conditional) {
        skip_site = e->n;
        a64_bcond(e, a64_invert_cond((a64_cond_t)cond), 0);
    }

    if (result == TR_ENDS_BLOCK) {
        emit_branch(t, insn);
    } else if ((insn & 0x0c000000u) == 0x04000000u) {
        emit_ldst(t, insn);
    } else {
        emit_dp(t, insn, &o);
    }

    if (conditional) {
        a64_bind(e, skip_site);
        if (result == TR_ENDS_BLOCK) {
            /* Condition failed: the branch was not taken, so the block ends
             * at the following instruction. Chaining will later patch both
             * edges independently (docs/dynarec.md §3.5). */
            emit_exit(t, t->pc + 4u, JIT_EXIT_NEXT, t->index + 1u);
        }
    }
    return result;
}

bool jit_translate(arm_cpu_t *cpu, uint32_t va, uint32_t *code,
                   size_t cap_words, jit_block_t *out) {
    jit_t     t;
    uint32_t  pa;
    jit_end_t end = JIT_END_LIMIT;
    unsigned  i;

    memset(out, 0, sizeof *out);
    memset(&t, 0, sizeof t);
    t.cpu  = cpu;
    t.priv = jit_priv(cpu);
    t.pc   = va;

    out->key.va    = va;
    out->key.flags = (uint16_t)(((cpu->cpsr & ARM_CPSR_T) ? JIT_BLK_THUMB : 0u)
                                | (t.priv ? JIT_BLK_PRIV : 0u));
    out->code       = code;
    out->end_reason = JIT_END_FALLBACK;

    t.thumb = (out->key.flags & JIT_BLK_THUMB) != 0;
    /* The escape hatch for a Thumb-specific divergence: one run with the whole
     * Thumb decoder off, which must leave the boot bit-identical. */
    if (t.thumb && (g_deny & JIT_DENY_THUMB)) return false;

    if (arm_mmu_translate(cpu, va, ARM_ACCESS_FETCH, t.priv, &pa)) {
        out->end_reason = JIT_END_FETCH_FAULT;
        return false;
    }
    out->key.pa = pa;

    a64_init(&t.e, code, cap_words);
    emit_prologue(&t);

    for (;;) {
        uint32_t ipa;
        tr_result_t r;

        if (t.index >= JIT_MAX_INSNS)                 { end = JIT_END_LIMIT; break; }
        if (t.index > 0 && (t.pc & 0xfffu) == 0)      { end = JIT_END_PAGE;  break; }
        if (t.e.n + JIT_INSN_HEADROOM + JIT_EPILOGUE_WORDS > t.e.cap)
                                                      { end = JIT_END_CODE_FULL; break; }
        if (arm_mmu_translate(cpu, t.pc, ARM_ACCESS_FETCH, t.priv, &ipa)) {
            end = JIT_END_FETCH_FAULT; break;
        }

        /* A 16-bit Thumb instruction is 2-byte aligned, so it can never
         * straddle the 4 KB boundary the check above stops at. */
        if (t.thumb) r = translate_thumb_one(&t, cpu->bus->read16(cpu->bus->ctx, ipa));
        else         r = translate_one(&t, cpu->bus->read32(cpu->bus->ctx, ipa));

        if (r == TR_FALLBACK) { end = JIT_END_FALLBACK; break; }
        if (!a64_ok(&t.e))    { out->end_reason = JIT_END_CODE_FULL; return false; }

        t.index++;
        out->native_count++;
        t.pc += t.thumb ? 2u : 4u;
        if (r == TR_ENDS_BLOCK) { end = JIT_END_BRANCH; break; }
    }

    out->insn_count = t.index;
    out->end_reason = end;
    if (t.index == 0) return false;

    if (end != JIT_END_BRANCH) {
        /* A fetch fault or an instruction we do not handle must be executed by
         * the interpreter; the other reasons just mean "look up the next
         * block". */
        unsigned reason = (end == JIT_END_FALLBACK || end == JIT_END_FETCH_FAULT)
                          ? (unsigned)JIT_EXIT_INTERPRET : (unsigned)JIT_EXIT_NEXT;
        emit_exit(&t, t.pc, reason, t.index);
    }

    for (i = 0; i < t.n_epi; i++) a64_bind(&t.e, t.epi[i]);
    emit_epilogue(&t);

    if (!a64_ok(&t.e)) { out->insn_count = 0; out->end_reason = JIT_END_CODE_FULL; return false; }
    out->code_words = t.e.n;
    return true;
}

/*
 * iOS3-VM — ARMv6 -> arm64 block translator (stage J2 skeleton).
 *
 * Translates a straight-line run of ARM instructions into arm64, stopping at
 * the first thing it does not handle and leaving that instruction to the
 * interpreter. See core/include/jit.h for the structural rule and
 * docs/dynarec.md for the design this implements.
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
 * takes the data abort through take_exception(). Because only the
 * no-writeback form (P == 1, W == 0) is translated, the guest has observed
 * nothing at that point, so re-execution is exact. The consequence is that
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
static uint64_t jit_helper_load32(arm_cpu_t *c, uint32_t va) {
    uint32_t pa;
    /* Same page-crossing split as the interpreter's mem_r32: a word straddling
     * a 4 KB boundary lives in two pages that need translating separately. */
    if (((va & 0xfffu) + 4u) > 0x1000u) {
        uint32_t val = 0, page_pa = 0;
        for (unsigned i = 0; i < 4u; i++) {
            uint32_t a = va + i;
            if (i == 0u || (a & 0xfffu) == 0u) {
                if (arm_mmu_translate(c, a, ARM_ACCESS_READ, jit_priv(c), &pa))
                    return (uint64_t)1 << 32;
                page_pa = pa & ~0xfffu;
            }
            val |= (uint32_t)c->bus->read8(c->bus->ctx, page_pa | (a & 0xfffu))
                   << (8u * i);
        }
        return val;
    }
    if (arm_mmu_translate(c, va, ARM_ACCESS_READ, jit_priv(c), &pa))
        return (uint64_t)1 << 32;
    return c->bus->read32(c->bus->ctx, pa);
}
static uint64_t jit_helper_load8(arm_cpu_t *c, uint32_t va) {
    uint32_t pa;
    if (arm_mmu_translate(c, va, ARM_ACCESS_READ, jit_priv(c), &pa))
        return (uint64_t)1 << 32;
    return c->bus->read8(c->bus->ctx, pa);
}
static uint32_t jit_helper_store32(arm_cpu_t *c, uint32_t va, uint32_t val) {
    uint32_t pa;
    if (((va & 0xfffu) + 4u) > 0x1000u) {
        uint32_t page_pa = 0;
        for (unsigned i = 0; i < 4u; i++) {
            uint32_t a = va + i;
            if (i == 0u || (a & 0xfffu) == 0u) {
                if (arm_mmu_translate(c, a, ARM_ACCESS_WRITE, jit_priv(c), &pa))
                    return 1u;
                page_pa = pa & ~0xfffu;
            }
            c->bus->write8(c->bus->ctx, page_pa | (a & 0xfffu),
                           (uint8_t)(val >> (8u * i)));
        }
        return 0u;
    }
    if (arm_mmu_translate(c, va, ARM_ACCESS_WRITE, jit_priv(c), &pa)) return 1u;
    c->bus->write32(c->bus->ctx, pa, val);
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

/*
 * N and Z from the result, C from the shifter carry-out, V preserved — the
 * A32 logical-flag rule that A64's ANDS does not implement (§5.2(1)).
 */
static void emit_logic_flags(jit_t *t, unsigned wres, const op2_t *o) {
    a64_emit_t *e = &t->e;
    if (o->carry_unaffected) {
        /* C keeps its incoming value; V is preserved. Save both, recompute
         * N/Z, splice them back. */
        a64_mrs_nzcv(e, JIT_HOST_S0);
        a64_ubfx(e, A64_X, JIT_HOST_S0, JIT_HOST_S0, 28, 2);
        a64_ands_reg(e, A64_W, A64_ZR, wres, wres, A64_LSL, 0);
        a64_mrs_nzcv(e, JIT_HOST_S1);
        a64_bfi(e, A64_X, JIT_HOST_S1, JIT_HOST_S0, 28, 2);
        a64_msr_nzcv(e, JIT_HOST_S1);
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

static void emit_ldst(jit_t *t, uint32_t insn) {
    a64_emit_t *e = &t->e;
    bool     U  = (insn >> 23) & 1u, B = (insn >> 22) & 1u, L = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
    uint32_t imm = insn & 0xfffu;
    unsigned wbase;
    size_t   ok_site;
    uintptr_t helper;

    /* cpu->r[15] must be exact at any faultable instruction, because the
     * interpreter re-executes this one on a fault and LR_abt is pc + 8. */
    a64_mov_imm(e, A64_W, JIT_HOST_PCOUT, t->pc);
    a64_str_uimm(e, A64_SZ_W, JIT_HOST_PCOUT, JIT_HOST_CPU, OFF_R(15));

    wbase = load_greg(t, rn, JIT_HOST_S0);
    if (U) a64_add_imm(e, A64_W, 1, wbase, imm, false);
    else   a64_sub_imm(e, A64_W, 1, wbase, imm, false);

    if (!L) {
        unsigned wval = load_greg(t, rd, JIT_HOST_S1);
        a64_mov_reg(e, A64_W, 2, wval);
    }

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
        if (L) u.ld = B ? jit_helper_load8  : jit_helper_load32;
        else   u.st = B ? jit_helper_store8 : jit_helper_store32;
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
    if (L) a64_tbz(e, 0, 32, 0);
    else   a64_cbz(e, A64_W, 0, 0);
    emit_exit(t, t->pc, JIT_EXIT_ABORT, t->index);
    a64_bind(e, ok_site);

    if (L) {
        unsigned wd = dest_greg(rd);
        a64_mov_reg(e, A64_W, wd, 0);
        commit_greg(t, rd, wd);
    }
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

/* ----------------------------------------------------------- dispatcher -- */

typedef enum { TR_FALLBACK = 0, TR_CONTINUE, TR_ENDS_BLOCK } tr_result_t;

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

    /* Thumb is 69% of the real workload and is stage J4; until then every
     * Thumb block is the interpreter's. */
    if (out->key.flags & JIT_BLK_THUMB) return false;

    if (arm_mmu_translate(cpu, va, ARM_ACCESS_FETCH, t.priv, &pa)) {
        out->end_reason = JIT_END_FETCH_FAULT;
        return false;
    }
    out->key.pa = pa;

    a64_init(&t.e, code, cap_words);
    emit_prologue(&t);

    for (;;) {
        uint32_t ipa, insn;
        tr_result_t r;

        if (t.index >= JIT_MAX_INSNS)                 { end = JIT_END_LIMIT; break; }
        if (t.index > 0 && (t.pc & 0xfffu) == 0)      { end = JIT_END_PAGE;  break; }
        if (t.e.n + JIT_INSN_HEADROOM + JIT_EPILOGUE_WORDS > t.e.cap)
                                                      { end = JIT_END_CODE_FULL; break; }
        if (arm_mmu_translate(cpu, t.pc, ARM_ACCESS_FETCH, t.priv, &ipa)) {
            end = JIT_END_FETCH_FAULT; break;
        }
        insn = cpu->bus->read32(cpu->bus->ctx, ipa);

        r = translate_one(&t, insn);
        if (r == TR_FALLBACK) { end = JIT_END_FALLBACK; break; }
        if (!a64_ok(&t.e))    { out->end_reason = JIT_END_CODE_FULL; return false; }

        t.index++;
        out->native_count++;
        t.pc += 4u;
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

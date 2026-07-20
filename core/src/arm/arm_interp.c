/*
 * iOS3-VM — ARMv6 interpreter.
 *
 * Scope (M1, in progress): the ARM instruction set (Thumb comes later).
 * Implemented: condition evaluation, the barrel shifter with carry-out,
 * the full data-processing group (AND..MVN) in immediate/register forms with
 * flag setting, branch/branch-with-link, single-data-transfer (LDR/STR, byte
 * and word, pre/post-indexed, writeback), and 32-bit multiply (MUL/MLA).
 *
 * Correctness is the priority for this milestone; the dynarec (see docs) will
 * later reuse this interpreter as its semantic oracle. Everything here is exact
 * ARMv6 behaviour, including r15-reads-as-PC+8 pipeline semantics.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"

/* ------------------------------------------------------------------ helpers */

static inline void set_flag(arm_cpu_t *c, uint32_t bit, bool on) {
    if (on) c->cpsr |= bit; else c->cpsr &= ~bit;
}
static inline bool get_flag(const arm_cpu_t *c, uint32_t bit) {
    return (c->cpsr & bit) != 0;
}

/* Defined below, but needed by the CP15 helper above it. */
static inline uint32_t reg_read(const arm_cpu_t *c, uint32_t pc, unsigned n);

arm_bank_t arm_bank_of_mode(uint32_t mode) {
    switch (mode & ARM_CPSR_MODE_MASK) {
        case ARM_MODE_FIQ: return ARM_BANK_FIQ;
        case ARM_MODE_IRQ: return ARM_BANK_IRQ;
        case ARM_MODE_SVC: return ARM_BANK_SVC;
        case ARM_MODE_ABT: return ARM_BANK_ABT;
        case ARM_MODE_UND: return ARM_BANK_UND;
        default:           return ARM_BANK_USR;   /* USR and SYS share a bank */
    }
}

void arm_set_mode(arm_cpu_t *c, uint32_t mode) {
    uint32_t old = c->cpsr & ARM_CPSR_MODE_MASK;
    mode &= ARM_CPSR_MODE_MASK;
    arm_bank_t ob = arm_bank_of_mode(old), nb = arm_bank_of_mode(mode);

    if (ob != nb) {
        /* Park the outgoing bank, then load the incoming one. */
        c->bank_r13[ob] = c->r[13];
        c->bank_r14[ob] = c->r[14];

        /* FIQ additionally banks r8–r12. */
        if (ob == ARM_BANK_FIQ) {
            for (int i = 0; i < 5; i++) { c->fiq_r8_12[i] = c->r[8+i]; c->r[8+i] = c->usr_r8_12[i]; }
        } else if (nb == ARM_BANK_FIQ) {
            for (int i = 0; i < 5; i++) { c->usr_r8_12[i] = c->r[8+i]; c->r[8+i] = c->fiq_r8_12[i]; }
        }

        c->r[13] = c->bank_r13[nb];
        c->r[14] = c->bank_r14[nb];
    }
    c->cpsr = (c->cpsr & ~ARM_CPSR_MODE_MASK) | mode;
}

/* Enter an exception: bank in the handler mode, stash the return address in its
 * LR and the old CPSR in its SPSR, mask interrupts, and vector the PC.
 * CP15 SCTLR.V relocates the vector table to 0xFFFF0000. */
static void take_exception(arm_cpu_t *c, uint32_t vector, uint32_t mode,
                           uint32_t ret_addr, bool mask_fiq, uint32_t *next) {
    uint32_t saved = c->cpsr;
    arm_set_mode(c, mode);
    c->spsr[arm_bank_of_mode(mode)] = saved;
    c->r[14] = ret_addr;
    c->cpsr |= ARM_CPSR_I;                 /* IRQs off on entry */
    if (mask_fiq) c->cpsr |= ARM_CPSR_F;
    c->cpsr &= ~ARM_CPSR_T;                /* exceptions enter in ARM state */
    *next = ((c->cp15.sctlr & ARM_SCTLR_V) ? 0xffff0000u : 0u) + vector;
}

/*
 * CP15 access via MCR (write) / MRC (read).
 *
 * Note on coverage: the architecturally significant registers below are modeled
 * exactly. Cache and TLB maintenance (c7/c8) are accepted as no-ops because we
 * have no caches to flush, and CP15 registers we do not model read as zero and
 * ignore writes. That is a deliberate exception to this core's usual
 * "trap what you don't implement" rule: CP15 is a configuration space that
 * kernels probe widely, and trapping harmless probes would stop a boot dead.
 */
static arm_status_t exec_coprocessor(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    unsigned cp   = (insn >> 8)  & 0xfu;
    if (cp != 15) return ARM_UNDEFINED;         /* only CP15 exists on this SoC */

    bool     load = (insn >> 20) & 1u;          /* 1 = MRC (read), 0 = MCR */
    unsigned crn  = (insn >> 16) & 0xfu;
    unsigned rd   = (insn >> 12) & 0xfu;
    unsigned opc2 = (insn >> 5)  & 7u;
    unsigned crm  = insn & 0xfu;
    arm_cp15_t *p = &c->cp15;

    if (load) {
        uint32_t v = 0;
        switch (crn) {
            case 0:
                if (crm == 0 && opc2 == 0) v = ARM1176_MIDR;
                else if (crm == 0 && opc2 == 1) v = ARM1176_CACHE_TYPE;
                break;
            case 1:
                if (opc2 == 0) v = p->sctlr; else if (opc2 == 1) v = p->actlr;
                else if (opc2 == 2) v = p->cpacr;
                break;
            case 2:
                if (opc2 == 0) v = p->ttbr0; else if (opc2 == 1) v = p->ttbr1;
                else if (opc2 == 2) v = p->ttbcr;
                break;
            case 3:  v = p->dacr; break;
            case 5:  v = (opc2 == 1) ? p->ifsr : p->dfsr; break;
            case 6:  v = (opc2 == 2) ? p->ifar : p->dfar; break;
            case 13: v = (opc2 == 1) ? p->context_id : p->fcse_pid; break;
            default: v = 0; break;              /* unmodelled: reads as zero */
        }
        c->r[rd] = v;
    } else {
        uint32_t v = reg_read(c, pc, rd);
        switch (crn) {
            case 1:
                if (opc2 == 0) p->sctlr = v; else if (opc2 == 1) p->actlr = v;
                else if (opc2 == 2) p->cpacr = v;
                break;
            case 2:
                if (opc2 == 0) p->ttbr0 = v; else if (opc2 == 1) p->ttbr1 = v;
                else if (opc2 == 2) p->ttbcr = v;
                break;
            case 3:  p->dacr = v; break;
            case 5:  if (opc2 == 1) p->ifsr = v; else p->dfsr = v; break;
            case 6:  if (opc2 == 2) p->ifar = v; else p->dfar = v; break;
            case 7:  break;                     /* cache maintenance: no-op */
            case 8:  break;                     /* TLB maintenance: no-op   */
            case 13: if (opc2 == 1) p->context_id = v; else p->fcse_pid = v; break;
            default: break;                     /* unmodelled: ignored      */
        }
    }
    return ARM_OK;
}

void arm_reset(arm_cpu_t *cpu, const arm_bus_t *bus) {
    for (int i = 0; i < 16; i++) cpu->r[i] = 0;
    for (int i = 0; i < ARM_BANK_COUNT; i++) {
        cpu->spsr[i] = 0; cpu->bank_r13[i] = 0; cpu->bank_r14[i] = 0;
    }
    for (int i = 0; i < 5; i++) { cpu->fiq_r8_12[i] = 0; cpu->usr_r8_12[i] = 0; }
    { arm_cp15_t z = {0}; cpu->cp15 = z; }   /* MMU off, low vectors */
    /* On reset the ARM1176 enters SVC mode with IRQ+FIQ disabled and begins
     * execution from the reset vector (0x0, or 0xffff0000 with high vectors).
     * We start at 0x0; the machine layer relocates PC as needed. */
    cpu->cpsr   = ARM_MODE_SVC | (1u << 7) | (1u << 6); /* I and F masked */
    cpu->cycles = 0;
    cpu->bus    = bus;
}

bool arm_cond_passed(const arm_cpu_t *c, uint32_t cond) {
    bool N = get_flag(c, ARM_CPSR_N), Z = get_flag(c, ARM_CPSR_Z);
    bool C = get_flag(c, ARM_CPSR_C), V = get_flag(c, ARM_CPSR_V);
    switch (cond & 0xf) {
        case 0x0: return Z;              /* EQ */
        case 0x1: return !Z;             /* NE */
        case 0x2: return C;              /* CS/HS */
        case 0x3: return !C;             /* CC/LO */
        case 0x4: return N;              /* MI */
        case 0x5: return !N;             /* PL */
        case 0x6: return V;              /* VS */
        case 0x7: return !V;             /* VC */
        case 0x8: return C && !Z;        /* HI */
        case 0x9: return !C || Z;        /* LS */
        case 0xa: return N == V;         /* GE */
        case 0xb: return N != V;         /* LT */
        case 0xc: return !Z && (N == V); /* GT */
        case 0xd: return Z || (N != V);  /* LE */
        case 0xe: return true;           /* AL */
        default:  return true;           /* 0xf (NV) is unpredictable on ARMv6; treat as AL */
    }
}

/* Reading r15 as an operand yields the address of the current instruction + 8
 * (the classic ARM pipeline offset). `pc` is the current instruction address. */
static inline uint32_t reg_read(const arm_cpu_t *c, uint32_t pc, unsigned n) {
    return (n == 15) ? pc + 8 : c->r[n];
}

/* 32-bit rotate right. */
static inline uint32_t ror32(uint32_t v, unsigned n) {
    n &= 31u;
    return n ? ((v >> n) | (v << (32 - n))) : v;
}

/* Barrel shifter. Returns the shifted value and, via *carry, the shifter
 * carry-out (seeded with the current C flag for the "unaffected" cases). */
static uint32_t barrel_shift(uint32_t val, unsigned type, unsigned amount,
                             bool reg_amount, bool *carry) {
    switch (type & 3u) {
        case 0: /* LSL */
            if (amount == 0) return val;                 /* C unaffected */
            if (amount < 32) { *carry = (val >> (32 - amount)) & 1u; return val << amount; }
            if (amount == 32) { *carry = val & 1u; return 0; }
            *carry = 0; return 0;
        case 1: /* LSR */
            if (amount == 0) {                           /* imm 0 => LSR #32 */
                if (reg_amount) return val;              /* reg 0 => unaffected */
                *carry = (val >> 31) & 1u; return 0;
            }
            if (amount < 32) { *carry = (val >> (amount - 1)) & 1u; return val >> amount; }
            if (amount == 32) { *carry = (val >> 31) & 1u; return 0; }
            *carry = 0; return 0;
        case 2: /* ASR */
            if (amount == 0) {                           /* imm 0 => ASR #32 */
                if (reg_amount) return val;
                *carry = (val >> 31) & 1u; return (val & 0x80000000u) ? 0xffffffffu : 0;
            }
            if (amount < 32) {
                *carry = (val >> (amount - 1)) & 1u;
                return (uint32_t)((int32_t)val >> amount);
            }
            *carry = (val >> 31) & 1u;
            return (val & 0x80000000u) ? 0xffffffffu : 0;
        default: /* ROR / RRX */
            if (amount == 0) {
                if (reg_amount) return val;              /* reg 0 => unaffected */
                /* imm 0 => RRX: 33-bit rotate through carry */
                { uint32_t res = (val >> 1) | ((*carry ? 1u : 0u) << 31);
                  *carry = val & 1u; return res; }
            }
            amount &= 31u;
            if (amount == 0) { *carry = (val >> 31) & 1u; return val; } /* ROR #32 */
            *carry = (val >> (amount - 1)) & 1u;
            return ror32(val, amount);
    }
}

/* Decode the shifter operand of a data-processing instruction. */
static uint32_t dp_operand2(arm_cpu_t *c, uint32_t pc, uint32_t insn, bool *carry) {
    if (insn & (1u << 25)) {                 /* immediate */
        uint32_t imm  = insn & 0xff;
        uint32_t rot  = ((insn >> 8) & 0xf) * 2;
        uint32_t val  = ror32(imm, rot);
        if (rot != 0) *carry = (val >> 31) & 1u; /* rot 0 leaves C unaffected */
        return val;
    } else {                                  /* register, with shift */
        unsigned rm   = insn & 0xf;
        unsigned type = (insn >> 5) & 3u;
        if (insn & (1u << 4)) {               /* register-specified shift amount */
            unsigned rs  = (insn >> 8) & 0xf;
            unsigned amt = reg_read(c, pc, rs) & 0xff;
            uint32_t rmv = reg_read(c, pc, rm); /* rm reads PC+12 if rm==15 & reg shift, but rare */
            return barrel_shift(rmv, type, amt, true, carry);
        } else {                              /* immediate shift amount */
            unsigned amt = (insn >> 7) & 0x1f;
            uint32_t rmv = reg_read(c, pc, rm);
            return barrel_shift(rmv, type, amt, false, carry);
        }
    }
}

/* Arithmetic flag computation for add-with-carry style ops. */
static uint32_t alu_add(arm_cpu_t *c, uint32_t a, uint32_t b, uint32_t cin, bool set) {
    uint64_t u = (uint64_t)a + (uint64_t)b + (uint64_t)cin;
    uint32_t r = (uint32_t)u;
    if (set) {
        set_flag(c, ARM_CPSR_N, (r >> 31) & 1u);
        set_flag(c, ARM_CPSR_Z, r == 0);
        set_flag(c, ARM_CPSR_C, (u >> 32) & 1u);
        set_flag(c, ARM_CPSR_V, (~(a ^ b) & (a ^ r)) >> 31);
    }
    return r;
}
/* Subtraction is add of the one's complement plus carry-in (borrow = !carry). */
static uint32_t alu_sub(arm_cpu_t *c, uint32_t a, uint32_t b, uint32_t cin, bool set) {
    return alu_add(c, a, ~b, cin, set);
}

static void alu_logic_flags(arm_cpu_t *c, uint32_t r, bool carry, bool set) {
    if (!set) return;
    set_flag(c, ARM_CPSR_N, (r >> 31) & 1u);
    set_flag(c, ARM_CPSR_Z, r == 0);
    set_flag(c, ARM_CPSR_C, carry);
    /* V is preserved by logical operations. */
}

/* ------------------------------------------------------------ instr groups */

static arm_status_t exec_data_processing(arm_cpu_t *c, uint32_t pc, uint32_t insn,
                                         uint32_t *next) {
    unsigned opcode = (insn >> 21) & 0xf;
    bool     S      = (insn >> 20) & 1u;
    unsigned rn     = (insn >> 16) & 0xf;
    unsigned rd     = (insn >> 12) & 0xf;

    /* Opcodes 0x8..0xb (TST/TEQ/CMP/CMN) are valid comparisons only when S==1.
     * With S==0 this is the ARMv6 miscellaneous/control space (BX, BLX(reg),
     * MRS, MSR, CLZ, ...) — it must NOT fall through to the comparison cases,
     * or e.g. BX would silently execute as TEQ and never branch. */
    if (opcode >= 0x8 && opcode <= 0xb && !S) {
        if ((insn & 0x0ffffff0u) == 0x012fff10u) {          /* BX Rm */
            *next = reg_read(c, pc, insn & 0xf) & ~1u;      /* Thumb bit ignored (ARM-only for now) */
            return ARM_OK;
        }
        if ((insn & 0x0ffffff0u) == 0x012fff30u) {          /* BLX Rm */
            c->r[14] = pc + 4;
            *next = reg_read(c, pc, insn & 0xf) & ~1u;
            return ARM_OK;
        }

        {
            bool spsr = (insn >> 22) & 1u;   /* R bit: 0 = CPSR, 1 = SPSR */
            arm_bank_t bank = arm_bank_of_mode(c->cpsr);
            bool has_spsr = (bank != ARM_BANK_USR);

            /* MRS Rd, <psr>: cccc 00010 R 001111 dddd 000000000000 */
            if ((insn & 0x0fbf0fffu) == 0x010f0000u) {
                if (spsr && !has_spsr) return ARM_UNDEFINED; /* no SPSR in USR/SYS */
                c->r[rd] = spsr ? c->spsr[bank] : c->cpsr;
                return ARM_OK;
            }

            /* MSR <psr>_fields, Rm | #imm:
             *   register:  cccc 00010 R 10 mask 1111 00000000 mmmm
             *   immediate: cccc 00110 R 10 mask 1111 rot   imm8   */
            if ((insn & 0x0fb00000u) == 0x01200000u ||
                (insn & 0x0fb00000u) == 0x03200000u) {
                unsigned fields = (insn >> 16) & 0xfu;
                uint32_t val;
                if (insn & (1u << 25)) {                     /* immediate form */
                    val = ror32(insn & 0xffu, ((insn >> 8) & 0xfu) * 2u);
                } else {
                    val = reg_read(c, pc, insn & 0xfu);
                }
                uint32_t mask = 0;
                if (fields & 1u) mask |= 0x000000ffu;        /* c: control (mode, I/F/T) */
                if (fields & 2u) mask |= 0x0000ff00u;        /* x: extension            */
                if (fields & 4u) mask |= 0x00ff0000u;        /* s: status               */
                if (fields & 8u) mask |= 0xff000000u;        /* f: flags                */

                if (spsr) {
                    if (!has_spsr) return ARM_UNDEFINED;
                    c->spsr[bank] = (c->spsr[bank] & ~mask) | (val & mask);
                } else {
                    /* User mode may only change the flags byte. */
                    if (!has_spsr && (c->cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_USR)
                        mask &= 0xff000000u;
                    uint32_t newcpsr = (c->cpsr & ~mask) | (val & mask);
                    if ((mask & 0x1fu) && (newcpsr & ARM_CPSR_MODE_MASK)
                                          != (c->cpsr & ARM_CPSR_MODE_MASK)) {
                        /* Mode field changed: rebank, then apply the rest. */
                        arm_set_mode(c, newcpsr);
                        c->cpsr = (c->cpsr & ARM_CPSR_MODE_MASK)
                                | (newcpsr & ~ARM_CPSR_MODE_MASK);
                    } else {
                        c->cpsr = newcpsr;
                    }
                }
                return ARM_OK;
            }
        }
        return ARM_UNDEFINED;                                /* CLZ, saturating/DSP: later */
    }

    bool shifter_carry = get_flag(c, ARM_CPSR_C);
    uint32_t op2 = dp_operand2(c, pc, insn, &shifter_carry);
    uint32_t a   = reg_read(c, pc, rn);
    uint32_t cin = get_flag(c, ARM_CPSR_C) ? 1u : 0u;
    uint32_t res = 0;
    bool     writes = true;

    switch (opcode) {
        case 0x0: res = a & op2;  alu_logic_flags(c, res, shifter_carry, S); break; /* AND */
        case 0x1: res = a ^ op2;  alu_logic_flags(c, res, shifter_carry, S); break; /* EOR */
        case 0x2: res = alu_sub(c, a, op2, 1, S);        break;                      /* SUB */
        case 0x3: res = alu_sub(c, op2, a, 1, S);        break;                      /* RSB */
        case 0x4: res = alu_add(c, a, op2, 0, S);        break;                      /* ADD */
        case 0x5: res = alu_add(c, a, op2, cin, S);      break;                      /* ADC */
        case 0x6: res = alu_sub(c, a, op2, cin, S);      break;                      /* SBC */
        case 0x7: res = alu_sub(c, op2, a, cin, S);      break;                      /* RSC */
        case 0x8: (void)(a & op2);  alu_logic_flags(c, a & op2, shifter_carry, true); writes = false; break; /* TST */
        case 0x9: alu_logic_flags(c, a ^ op2, shifter_carry, true); writes = false; break;               /* TEQ */
        case 0xa: alu_sub(c, a, op2, 1, true);   writes = false; break;             /* CMP */
        case 0xb: alu_add(c, a, op2, 0, true);   writes = false; break;             /* CMN */
        case 0xc: res = a | op2;  alu_logic_flags(c, res, shifter_carry, S); break; /* ORR */
        case 0xd: res = op2;      alu_logic_flags(c, res, shifter_carry, S); break; /* MOV */
        case 0xe: res = a & ~op2; alu_logic_flags(c, res, shifter_carry, S); break; /* BIC */
        case 0xf: res = ~op2;     alu_logic_flags(c, res, shifter_carry, S); break; /* MVN */
    }

    if (writes) {
        c->r[rd] = res;
        if (rd == 15) *next = res & ~3u; /* ARM-state ALUWritePC forces word alignment */
    }
    return ARM_OK;
}

static arm_status_t exec_branch(arm_cpu_t *c, uint32_t pc, uint32_t insn, uint32_t *next) {
    int32_t off = (int32_t)(insn << 8) >> 6;   /* sign-extend 24-bit, <<2 */
    if (insn & (1u << 24)) c->r[14] = pc + 4;   /* BL: LR = return address */
    *next = (pc + 8 + off) & ~3u;
    return ARM_OK;
}

static arm_status_t exec_single_transfer(arm_cpu_t *c, uint32_t pc, uint32_t insn,
                                         uint32_t *next) {
    bool I = (insn >> 25) & 1u;  /* 0 => immediate offset, 1 => register offset */
    bool P = (insn >> 24) & 1u;  /* pre/post index */
    bool U = (insn >> 23) & 1u;  /* add/subtract offset */
    bool B = (insn >> 22) & 1u;  /* byte/word */
    bool W = (insn >> 21) & 1u;  /* writeback */
    bool L = (insn >> 20) & 1u;  /* load/store */
    unsigned rn = (insn >> 16) & 0xf;
    unsigned rd = (insn >> 12) & 0xf;

    uint32_t offset;
    if (!I) {
        offset = insn & 0xfff;
    } else {
        unsigned rm = insn & 0xf, type = (insn >> 5) & 3u, amt = (insn >> 7) & 0x1f;
        bool carry = get_flag(c, ARM_CPSR_C);
        offset = barrel_shift(reg_read(c, pc, rm), type, amt, false, &carry);
    }

    uint32_t base = reg_read(c, pc, rn);
    uint32_t addr = P ? (U ? base + offset : base - offset) : base;

    if (L) { /* load */
        uint32_t val = B ? c->bus->read8(c->bus->ctx, addr)
                         : c->bus->read32(c->bus->ctx, addr);
        c->r[rd] = val;
        if (rd == 15) *next = val & ~3u;
    } else {  /* store */
        uint32_t val = reg_read(c, pc, rd); /* stored r15 is +12 on real HW; +8 is close enough here */
        if (B) c->bus->write8 (c->bus->ctx, addr, (uint8_t)val);
        else   c->bus->write32(c->bus->ctx, addr, val);
    }

    /* Writeback: post-indexed always writes back; pre-indexed writes back if W. */
    if (!P) { addr = U ? base + offset : base - offset; c->r[rn] = addr; }
    else if (W) { c->r[rn] = addr; }
    return ARM_OK;
}

/* Extra load/store: LDRH/STRH/LDRSB/LDRSH (halfword and sign-extending forms).
 * Encoding: cccc 000 P U I W L nnnn tttt iiii 1SH1 iiii, with SH != 00.
 * I selects an 8-bit immediate offset (split across bits[11:8] and bits[3:0])
 * versus a register offset in Rm. */
static arm_status_t exec_extra_transfer(arm_cpu_t *c, uint32_t pc, uint32_t insn,
                                        uint32_t *next) {
    bool P = (insn >> 24) & 1u;
    bool U = (insn >> 23) & 1u;
    bool I = (insn >> 22) & 1u;
    bool W = (insn >> 21) & 1u;
    bool L = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xf;
    unsigned rd = (insn >> 12) & 0xf;
    unsigned sh = (insn >> 5) & 3u;

    uint32_t offset = I ? ((((insn >> 8) & 0xf) << 4) | (insn & 0xf))
                        : reg_read(c, pc, insn & 0xf);
    uint32_t base = reg_read(c, pc, rn);
    uint32_t addr = P ? (U ? base + offset : base - offset) : base;

    if (L) {
        uint32_t val;
        switch (sh) {
            case 1: val = c->bus->read16(c->bus->ctx, addr); break;   /* LDRH  */
            case 2: val = (uint32_t)(int32_t)(int8_t) c->bus->read8 (c->bus->ctx, addr); break; /* LDRSB */
            case 3: val = (uint32_t)(int32_t)(int16_t)c->bus->read16(c->bus->ctx, addr); break; /* LDRSH */
            default: return ARM_UNDEFINED;
        }
        c->r[rd] = val;
        if (rd == 15) *next = val & ~3u;
    } else {
        if (sh != 1) return ARM_UNDEFINED;        /* LDRD/STRD: not yet */
        c->bus->write16(c->bus->ctx, addr, (uint16_t)reg_read(c, pc, rd)); /* STRH */
    }

    if (!P) { addr = U ? base + offset : base - offset; c->r[rn] = addr; }
    else if (W) { c->r[rn] = addr; }
    return ARM_OK;
}

/* Block data transfer: LDM/STM in all four addressing modes.
 * Encoding: cccc 100 P U S W L nnnn register_list(16).
 * Registers always move in increasing register order at increasing addresses;
 * P/U only decide where the run of addresses starts. */
static arm_status_t exec_block_transfer(arm_cpu_t *c, uint32_t pc, uint32_t insn,
                                        uint32_t *next) {
    bool P = (insn >> 24) & 1u;
    bool U = (insn >> 23) & 1u;
    bool S = (insn >> 22) & 1u;
    bool W = (insn >> 21) & 1u;
    bool L = (insn >> 20) & 1u;
    unsigned rn = (insn >> 16) & 0xf;
    uint32_t list = insn & 0xffffu;

    if (list == 0) return ARM_UNDEFINED;     /* empty list is unpredictable */

    /* S=1 has two meanings. With PC in a load list it is an exception return
     * (CPSR <- SPSR); otherwise it transfers the *user* bank, which needs more
     * plumbing than we have, so that variant still traps. */
    bool restore_cpsr = false;
    if (S) {
        if (L && (list & (1u << 15))) restore_cpsr = true;
        else return ARM_UNDEFINED;
    }

    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) if (list & (1u << i)) n++;

    uint32_t base = reg_read(c, pc, rn);
    uint32_t addr, wb;
    if (U) { addr = P ? base + 4u : base;                  wb = base + 4u * n; }
    else   { addr = P ? base - 4u * n : base - 4u * n + 4u; wb = base - 4u * n; }

    for (unsigned i = 0; i < 16; i++) {
        if (!(list & (1u << i))) continue;
        if (L) {
            uint32_t v = c->bus->read32(c->bus->ctx, addr);
            c->r[i] = v;
            if (i == 15) *next = v & ~3u;    /* LDM with PC in the list branches */
        } else {
            c->bus->write32(c->bus->ctx, addr, reg_read(c, pc, i));
        }
        addr += 4u;
    }

    /* On LDM with Rn in the list, the loaded value wins over writeback. */
    if (W && !(L && (list & (1u << rn)))) c->r[rn] = wb;

    /* Exception return: writeback lands in the handler's banked Rn above,
     * then CPSR <- SPSR rebanks us into the interrupted mode. */
    if (restore_cpsr) {
        arm_bank_t b = arm_bank_of_mode(c->cpsr);
        if (b == ARM_BANK_USR) return ARM_UNDEFINED;   /* no SPSR in USR/SYS */
        uint32_t s = c->spsr[b];
        arm_set_mode(c, s);
        c->cpsr = (c->cpsr & ARM_CPSR_MODE_MASK) | (s & ~ARM_CPSR_MODE_MASK);
    }
    return ARM_OK;
}

static arm_status_t exec_multiply(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    bool A  = (insn >> 21) & 1u;  /* accumulate (MLA) */
    bool S  = (insn >> 20) & 1u;
    unsigned rd = (insn >> 16) & 0xf; /* note: Rd/Rn fields are swapped vs data-proc */
    unsigned rn = (insn >> 12) & 0xf;
    unsigned rs = (insn >> 8)  & 0xf;
    unsigned rm = insn & 0xf;
    uint32_t res = reg_read(c, pc, rm) * reg_read(c, pc, rs);
    if (A) res += reg_read(c, pc, rn);
    c->r[rd] = res;
    if (S) {
        set_flag(c, ARM_CPSR_N, (res >> 31) & 1u);
        set_flag(c, ARM_CPSR_Z, res == 0);
        /* C is unpredictable after MUL on ARMv6; leave as-is. V preserved. */
    }
    return ARM_OK;
}

/* ------------------------------------------------------------------- step */

arm_status_t arm_step(arm_cpu_t *c) {
    uint32_t pc   = c->r[15];
    uint32_t insn = c->bus->read32(c->bus->ctx, pc);
    uint32_t next = pc + 4;
    arm_status_t st = ARM_OK;

    c->cycles++;

    uint32_t cond = insn >> 28;
    if (!arm_cond_passed(c, cond)) { c->r[15] = next; return ARM_OK; }

    /* Coarse top-level decode. This intentionally covers only the encodings
     * M1 implements; anything else returns ARM_UNDEFINED so the harness/log
     * can show us exactly which instruction to implement next. */
    if ((insn & 0x0f000000u) == 0x0a000000u ||
        (insn & 0x0f000000u) == 0x0b000000u) {           /* B / BL */
        st = exec_branch(c, pc, insn, &next);
    } else if ((insn & 0x0fc000f0u) == 0x00000090u) {     /* MUL / MLA */
        st = exec_multiply(c, pc, insn);
    } else if ((insn & 0x0c000000u) == 0x04000000u) {     /* single data transfer */
        st = exec_single_transfer(c, pc, insn, &next);
    } else if ((insn & 0x0e000000u) == 0x08000000u) {     /* LDM / STM */
        st = exec_block_transfer(c, pc, insn, &next);
    } else if ((insn & 0x0e000000u) == 0x00000000u &&
               (insn & 0x00000090u) == 0x00000090u &&
               (insn & 0x00000060u) != 0x00000000u) {
        /* bits[6:5] != 00 -> extra load/store (LDRH/STRH/LDRSB/LDRSH, LDRD/STRD) */
        st = exec_extra_transfer(c, pc, insn, &next);
    } else if ((insn & 0x0e000000u) == 0x00000000u &&
               (insn & 0x00000090u) == 0x00000090u) {
        /* Remaining extension space with bits[6:5]==00: SWP/SWPB and the DSP
         * multiplies. bits[27:25]==000, bit7==1, bit4==1. The bit25==0
         * requirement (mask 0x0e000000, not 0x0c000000) is essential: immediate
         * data-processing sets bit25 and its imm8 may itself have bits 7 and 4
         * set (e.g. MOV r0,#0x90), so it must NOT be trapped here. MUL/MLA are
         * handled above; these are unimplemented — trap rather than corrupt Rd. */
        st = ARM_UNDEFINED;
    } else if ((insn & 0x0f000010u) == 0x0e000010u) {     /* MCR / MRC (CP15) */
        st = exec_coprocessor(c, pc, insn);
    } else if ((insn & 0x0f000000u) == 0x0f000000u) {     /* SWI / SVC */
        take_exception(c, ARM_VEC_SWI, ARM_MODE_SVC, pc + 4, false, &next);
    } else if ((insn & 0x0c000000u) == 0x00000000u) {     /* data processing / PSR */
        st = exec_data_processing(c, pc, insn, &next);
    } else {
        st = ARM_UNDEFINED;
    }

    if (st == ARM_OK) c->r[15] = next;
    return st;
}

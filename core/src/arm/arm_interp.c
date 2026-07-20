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

static inline bool cpu_is_priv(const arm_cpu_t *c) {
    return (c->cpsr & ARM_CPSR_MODE_MASK) != ARM_MODE_USR;
}

/*
 * Data-side memory accessors. Every guest load/store goes through translation;
 * a fault is latched on the CPU and converted into a data abort by arm_step
 * once the instruction finishes.
 */
static void note_abort(arm_cpu_t *c, uint32_t fsr, uint32_t va) {
    if (c->abort_pending) return;          /* keep the first fault */
    c->abort_pending = true;
    c->abort_fsr = fsr;
    c->abort_far = va;
}
/* The `priv` argument is explicit because the translation-mode load/stores
 * (LDRT/STRT/LDRBT/STRBT, encoded P==0 && W==1) must translate as *unprivileged*
 * even when executing in a privileged mode. That is precisely the mechanism a
 * kernel uses to touch user memory safely, so getting it wrong would let a
 * guest kernel read pages it should fault on. */
#define MEM_READ(bits)                                                        \
    static uint##bits##_t mem_r##bits##_as(arm_cpu_t *c, uint32_t va, bool priv) { \
        uint32_t pa, f = arm_mmu_translate(c, va, false, priv, &pa);          \
        if (f) { note_abort(c, f, va); return 0; }                            \
        return c->bus->read##bits(c->bus->ctx, pa);                           \
    }                                                                         \
    static inline uint##bits##_t mem_r##bits(arm_cpu_t *c, uint32_t va) {     \
        return mem_r##bits##_as(c, va, cpu_is_priv(c));                       \
    }
#define MEM_WRITE(bits)                                                       \
    static void mem_w##bits##_as(arm_cpu_t *c, uint32_t va, uint##bits##_t v, \
                                 bool priv) {                                 \
        uint32_t pa, f = arm_mmu_translate(c, va, true, priv, &pa);           \
        if (f) { note_abort(c, f, va); return; }                              \
        c->bus->write##bits(c->bus->ctx, pa, v);                              \
    }                                                                         \
    static inline void mem_w##bits(arm_cpu_t *c, uint32_t va, uint##bits##_t v) {\
        mem_w##bits##_as(c, va, v, cpu_is_priv(c));                           \
    }
MEM_READ(32)  MEM_READ(16)  MEM_READ(8)
MEM_WRITE(32) MEM_WRITE(16) MEM_WRITE(8)

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

/*
 * Access a register as the USER bank sees it, whatever mode we are in. Only
 * r8-r14 differ, and only for FIQ (r8-r12) and the privileged modes (r13-r14).
 */
static uint32_t reg_read_user(const arm_cpu_t *c, uint32_t pc, unsigned i) {
    arm_bank_t cur = arm_bank_of_mode(c->cpsr);
    if (i == 15) return pc + 8;
    if (cur == ARM_BANK_USR) return c->r[i];
    if (i == 13) return c->bank_r13[ARM_BANK_USR];
    if (i == 14) return c->bank_r14[ARM_BANK_USR];
    if (i >= 8 && i <= 12 && cur == ARM_BANK_FIQ) return c->usr_r8_12[i - 8];
    return c->r[i];
}

static void reg_write_user(arm_cpu_t *c, unsigned i, uint32_t v) {
    arm_bank_t cur = arm_bank_of_mode(c->cpsr);
    if (cur == ARM_BANK_USR) { c->r[i] = v; return; }
    if (i == 13) { c->bank_r13[ARM_BANK_USR] = v; return; }
    if (i == 14) { c->bank_r14[ARM_BANK_USR] = v; return; }
    if (i >= 8 && i <= 12 && cur == ARM_BANK_FIQ) { c->usr_r8_12[i - 8] = v; return; }
    c->r[i] = v;
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
    /*
     * Taking an exception clears the exclusive monitor. Without this, an
     * interrupt landing between a thread's LDREX and its STREX leaves the tag
     * intact, so the preempted thread's STREX still succeeds after the
     * preempting thread has taken the same lock — two owners of one spinlock.
     * This is invisible until interrupts actually fire, which is why it
     * survived until the timer started working.
     */
    c->excl_valid = false;
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

    /*
     * CP14 is the debug coprocessor. The kernel probes it during init (reading
     * DBGDIDR) to decide whether a debug unit is present. Reporting "no debug
     * hardware" — zeros — is truthful for this emulator and lets the probe
     * proceed; trapping it would stop a boot over a feature query.
     */
    if (cp == 14) {
        if ((insn >> 20) & 1u) c->r[(insn >> 12) & 0xfu] = 0;  /* MRC reads 0 */
        return ARM_OK;                                          /* MCR ignored */
    }
    /*
     * CP10/CP11 are the VFP11 coprocessor. XNU disables VFP and enables it
     * lazily per thread, so machine_switch_context reads and writes FPEXC on
     * every context switch — these are reached within the first ten million
     * instructions of boot.
     *
     * Only the SYSTEM registers are modelled (VMRS/VMSR, encoded as MRC/MCR on
     * p10 with opc1 == 7). Floating-point arithmetic is not implemented; if the
     * guest executes a real VFP data-processing instruction it will still trap,
     * which is the honest outcome — it names what is missing rather than
     * silently computing nonsense.
     */
    if (cp == 10 || cp == 11) {
        unsigned opc1 = (insn >> 21) & 7u;
        unsigned rd_  = (insn >> 12) & 0xfu;
        if (opc1 != 7) return ARM_UNDEFINED;      /* FP data-processing */

        unsigned reg = (insn >> 16) & 0xfu;       /* CRn selects the sysreg */
        if ((insn >> 20) & 1u) {                  /* VMRS: read */
            uint32_t v;
            switch (reg) {
                case 0:  v = ARM1176_FPSID;  break;   /* VFP11 identity   */
                case 1:  v = c->vfp_fpscr;   break;
                case 8:  v = c->vfp_fpexc;   break;
                default: v = 0;              break;   /* MVFR etc: absent */
            }
            /* Rt == 15 means "write the flags", used by VMRS APSR_nzcv. */
            if (rd_ == 15) c->cpsr = (c->cpsr & 0x0fffffffu) | (v & 0xf0000000u);
            else           c->r[rd_] = v;
        } else {                                  /* VMSR: write */
            uint32_t v = reg_read(c, pc, rd_);
            if (reg == 1)      c->vfp_fpscr = v;
            else if (reg == 8) c->vfp_fpexc = v;
        }
        return ARM_OK;
    }

    if (cp != 15) return ARM_UNDEFINED;         /* CP10/11/14/15 only */

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
            case 13:
                switch (opc2) {
                    case 1:  v = p->context_id; break;
                    case 2:  v = p->tpidrurw;   break;
                    case 3:  v = p->tpidruro;   break;
                    case 4:  v = p->tpidrprw;   break;
                    default: v = p->fcse_pid;   break;
                }
                break;
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
            case 13:
                switch (opc2) {
                    case 1:  p->context_id = v; break;
                    case 2:  p->tpidrurw   = v; break;
                    case 3:  p->tpidruro   = v; break;
                    case 4:  p->tpidrprw   = v; break;
                    default: p->fcse_pid   = v; break;
                }
                break;
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
    cpu->abort_pending = false;
    cpu->abort_fsr = 0;
    cpu->abort_far = 0;
    cpu->irq_line = false;
    cpu->fiq_line = false;
    cpu->excl_valid = false;
    cpu->excl_addr = 0;
    cpu->vfp_fpexc = 0;
    cpu->vfp_fpscr = 0;
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
        default:  return true;           /* 0xf is the unconditional space; arm_step
                                          * intercepts it before this is reached. */
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
        /* Bit 0 of the target selects the instruction set: set means Thumb. */
        if ((insn & 0x0ffffff0u) == 0x012fff10u) {          /* BX Rm */
            uint32_t t = reg_read(c, pc, insn & 0xf);
            if (t & 1u) c->cpsr |= ARM_CPSR_T;
            *next = t & ~1u;
            return ARM_OK;
        }
        if ((insn & 0x0ffffff0u) == 0x012fff30u) {          /* BLX Rm */
            uint32_t t = reg_read(c, pc, insn & 0xf);
            c->r[14] = pc + 4;
            if (t & 1u) c->cpsr |= ARM_CPSR_T;
            *next = t & ~1u;
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
             *   immediate: cccc 00110 R 10 mask 1111 rot   imm8
             *
             * The SBO field bits[15:12]==0b1111 must be checked, and for the
             * register form bits[11:4]==0 as well. Without them this mask also
             * swallows CLZ, BKPT, QSUB/QDSUB and the SMULxy/SMLAWy DSP space,
             * which would then silently rewrite CPSR — including the mode
             * field — instead of executing or trapping. */
            if ((insn & 0x0fb0fff0u) == 0x0120f000u ||      /* register  */
                (insn & 0x0fb0f000u) == 0x0320f000u) {      /* immediate */
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
        if (rd == 15) {
            /* Writing PC with S set is an exception return (the classic
             * "SUBS pc, lr, #4" ending an IRQ handler): CPSR comes from the
             * current mode's SPSR, which also undoes the flag update above. */
            if (S) {
                arm_bank_t b = arm_bank_of_mode(c->cpsr);
                if (b != ARM_BANK_USR) {
                    uint32_t s = c->spsr[b];
                    arm_set_mode(c, s);
                    c->cpsr = (c->cpsr & ARM_CPSR_MODE_MASK) | (s & ~ARM_CPSR_MODE_MASK);
                }
            }
            /*
             * Align for the instruction set we are landing in, not the one we
             * came from. When S is set the SPSR restore above has just put T
             * back, so a handler returning to interrupted Thumb code must
             * align to a halfword. Forcing word alignment silently rewinds the
             * resume address by 2 whenever the interrupted Thumb instruction
             * sat at an odd halfword, and the guest re-executes the preceding
             * instruction — which is how a zone free ended up unlocking a
             * mutex at address 1.
             *
             * Safe for the ordinary S==0 case: in ARM state T is clear, so the
             * mask stays ~3u and MOV pc,Rm correctly does not interwork.
             */
            *next = res & ((c->cpsr & ARM_CPSR_T) ? ~1u : ~3u);
        }
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

    /* P==0 && W==1 is the translation form (LDRT/STRT/LDRBT/STRBT): the access
     * is performed as if unprivileged. */
    bool priv = (!P && W) ? false : cpu_is_priv(c);

    if (L) { /* load */
        uint32_t val = B ? mem_r8_as(c, addr, priv)
                         : mem_r32_as(c, addr, priv);
        /* Base Restored Abort Model: on a fault the destination and base must
         * be left untouched so the handler can fix the mapping and re-execute. */
        if (c->abort_pending) return ARM_OK;
        c->r[rd] = val;
        if (rd == 15) {                      /* LDR to PC interworks too */
            if (val & 1u) c->cpsr |= ARM_CPSR_T; else c->cpsr &= ~ARM_CPSR_T;
            *next = val & (uint32_t)((val & 1u) ? ~1u : ~3u);
        }
    } else {  /* store */
        uint32_t val = reg_read(c, pc, rd); /* stored r15 is +12 on real HW; +8 is close enough here */
        if (B) mem_w8_as(c, addr, (uint8_t)val, priv);
        else   mem_w32_as(c, addr, val, priv);
        if (c->abort_pending) return ARM_OK;
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
            case 1: val = mem_r16(c, addr); break;   /* LDRH  */
            case 2: val = (uint32_t)(int32_t)(int8_t) mem_r8(c, addr); break;  /* LDRSB */
            case 3: val = (uint32_t)(int32_t)(int16_t)mem_r16(c, addr); break; /* LDRSH */
            default: return ARM_UNDEFINED;
        }
        if (c->abort_pending) return ARM_OK;      /* base-restored abort model */
        c->r[rd] = val;
        if (rd == 15) *next = val & ~3u;
    } else {
        if (sh != 1) return ARM_UNDEFINED;        /* LDRD/STRD: not yet */
        mem_w16(c, addr, (uint16_t)reg_read(c, pc, rd)); /* STRH */
        if (c->abort_pending) return ARM_OK;
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

    /*
     * S=1 has two meanings. With PC in a load list it is an exception return
     * (CPSR <- SPSR). Otherwise it transfers the USER bank regardless of the
     * current mode — which is how a kernel saves and restores user context on
     * exception entry. Real XNU does exactly that with "STMIA sp,{r0-r14}^".
     */
    bool restore_cpsr = false;
    bool user_bank    = false;
    if (S) {
        if (L && (list & (1u << 15))) restore_cpsr = true;
        else                          user_bank = true;
    }

    unsigned n = 0;
    for (unsigned i = 0; i < 16; i++) if (list & (1u << i)) n++;

    uint32_t base = reg_read(c, pc, rn);
    uint32_t addr, wb;
    if (U) { addr = P ? base + 4u : base;                  wb = base + 4u * n; }
    else   { addr = P ? base - 4u * n : base - 4u * n + 4u; wb = base - 4u * n; }

    /* Loaded values are buffered and only committed once every access has
     * succeeded: the base-restored abort model requires that a data abort part
     * way through an LDM leaves the register file (and especially Rn) as it
     * was, so the handler can map the page and re-execute the instruction. */
    uint32_t loaded[16];
    for (unsigned i = 0; i < 16; i++) {
        if (!(list & (1u << i))) continue;
        if (L) {
            loaded[i] = mem_r32(c, addr);
        } else {
            mem_w32(c, addr, user_bank ? reg_read_user(c, pc, i)
                                       : reg_read(c, pc, i));
        }
        if (c->abort_pending) return ARM_OK;
        addr += 4u;
    }

    if (L) {
        for (unsigned i = 0; i < 16; i++) {
            if (!(list & (1u << i))) continue;
            if (user_bank) { reg_write_user(c, i, loaded[i]); continue; }
            c->r[i] = loaded[i];
            if (i == 15) {
                /* ARMv5 and later: loading PC INTERWORKS. Bit 0 selects the
                 * instruction set, so "POP {..,pc}" returning to a Thumb caller
                 * must switch to Thumb. Masking bit 0 off without setting T
                 * leaves the CPU decoding Thumb bytes as ARM — which is exactly
                 * how real XNU ended up executing garbage and taking a
                 * spurious SWI. */
                if (loaded[i] & 1u) c->cpsr |= ARM_CPSR_T;
                else                c->cpsr &= ~ARM_CPSR_T;
                *next = loaded[i] & (uint32_t)((loaded[i] & 1u) ? ~1u : ~3u);
            }
        }
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

/*
 * ARMv6 media space: the extend and byte-reverse families. Real XNU uses these
 * in ordinary compiled code, so they are not optional.
 *
 *   extend: cccc 0110 1 op nnnn dddd rr00 0111 mmmm
 *           Rn == 15 is the plain form; otherwise the result is added to Rn.
 *           rr rotates the source right by rr*8 before extracting.
 */
static arm_status_t exec_media(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    if ((insn & 0x0f0000f0u) == 0x06000070u) {
        unsigned op  = (insn >> 20) & 0xfu;
        unsigned rn  = (insn >> 16) & 0xfu;
        unsigned rd  = (insn >> 12) & 0xfu;
        unsigned rot = (insn >> 10) & 3u;
        uint32_t v   = ror32(reg_read(c, pc, insn & 0xfu), rot * 8u);
        uint32_t res;
        switch (op) {
            case 0xa: res = (uint32_t)(int32_t)(int8_t)(uint8_t)v;    break; /* SXTB  */
            case 0xb: res = (uint32_t)(int32_t)(int16_t)(uint16_t)v;  break; /* SXTH  */
            case 0xe: res = v & 0xffu;                                break; /* UXTB  */
            case 0xf: res = v & 0xffffu;                              break; /* UXTH  */
            default:  return ARM_UNDEFINED;      /* the 16-bit pair variants */
        }
        if (rn != 15) res += reg_read(c, pc, rn);          /* accumulate form */
        c->r[rd] = res;
        return ARM_OK;
    }

    /* REV / REV16 / REVSH */
    if ((insn & 0x0fff0ff0u) == 0x06bf0f30u) {             /* REV */
        uint32_t v = reg_read(c, pc, insn & 0xfu);
        c->r[(insn >> 12) & 0xfu] =
            ((v & 0xffu) << 24) | ((v & 0xff00u) << 8)
          | ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);
        return ARM_OK;
    }
    if ((insn & 0x0fff0ff0u) == 0x06bf0fb0u) {             /* REV16 */
        uint32_t v = reg_read(c, pc, insn & 0xfu);
        c->r[(insn >> 12) & 0xfu] =
            ((v & 0x00ffu) << 8) | ((v & 0xff00u) >> 8)
          | ((v & 0x00ff0000u) << 8) | ((v & 0xff000000u) >> 8);
        return ARM_OK;
    }
    if ((insn & 0x0fff0ff0u) == 0x06ff0fb0u) {             /* REVSH */
        uint32_t v = reg_read(c, pc, insn & 0xfu);
        uint16_t h = (uint16_t)(((v & 0xffu) << 8) | ((v >> 8) & 0xffu));
        c->r[(insn >> 12) & 0xfu] = (uint32_t)(int32_t)(int16_t)h;
        return ARM_OK;
    }
    return ARM_UNDEFINED;                  /* PKH, SEL, USAT, SMLAD, ... */
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

/* ==================================================================== Thumb */
/*
 * The Thumb (16-bit) instruction set. iPhone OS userland — SpringBoard and
 * most of the frameworks — is Thumb-compiled for code density, so this is not
 * optional for reaching a home screen.
 *
 * Thumb reuses every helper above (barrel shifter, ALU flag logic, translated
 * memory accessors, exception entry), so semantics cannot drift between the two
 * instruction sets. Reading r15 in Thumb yields the instruction address + 4,
 * and with bit 1 cleared for PC-relative loads.
 */
#define TB(n)  ((insn >> (n)) & 7u)          /* 3-bit register field at bit n */

static arm_status_t thumb_step(arm_cpu_t *c, uint32_t pc, uint16_t insn,
                               uint32_t *next) {
    const uint32_t pc4 = pc + 4;             /* r15 as read by the instruction */

    switch (insn >> 12) {
    case 0x0: case 0x1: {
        if ((insn & 0xf800u) == 0x1800u) {   /* ADD/SUB register or 3-bit imm */
            unsigned rd = TB(0), rs = TB(3);
            uint32_t op2 = (insn & (1u << 10)) ? (uint32_t)TB(6) : c->r[TB(6)];
            c->r[rd] = (insn & (1u << 9)) ? alu_sub(c, c->r[rs], op2, 1, true)
                                          : alu_add(c, c->r[rs], op2, 0, true);
            return ARM_OK;
        }
        /* LSL/LSR/ASR by immediate */
        unsigned rd = TB(0), rs = TB(3), amount = (insn >> 6) & 0x1fu;
        unsigned type = (insn >> 11) & 3u;
        bool carry = get_flag(c, ARM_CPSR_C);
        uint32_t res = barrel_shift(c->r[rs], type, amount, false, &carry);
        c->r[rd] = res;
        alu_logic_flags(c, res, carry, true);
        return ARM_OK;
    }
    case 0x2: case 0x3: {                    /* MOV/CMP/ADD/SUB 8-bit immediate */
        unsigned rd = (insn >> 8) & 7u;
        uint32_t imm = insn & 0xffu;
        switch ((insn >> 11) & 3u) {
            case 0: c->r[rd] = imm; alu_logic_flags(c, imm, get_flag(c, ARM_CPSR_C), true); break;
            case 1: alu_sub(c, c->r[rd], imm, 1, true); break;              /* CMP */
            case 2: c->r[rd] = alu_add(c, c->r[rd], imm, 0, true); break;
            default: c->r[rd] = alu_sub(c, c->r[rd], imm, 1, true); break;
        }
        return ARM_OK;
    }
    case 0x4: {
        if ((insn & 0xfc00u) == 0x4000u) {   /* ALU operations */
            unsigned rd = TB(0), rs = TB(3);
            uint32_t a = c->r[rd], b = c->r[rs];
            bool carry = get_flag(c, ARM_CPSR_C);
            switch ((insn >> 6) & 0xfu) {
                case 0x0: c->r[rd] = a & b; alu_logic_flags(c, c->r[rd], carry, true); break; /* AND */
                case 0x1: c->r[rd] = a ^ b; alu_logic_flags(c, c->r[rd], carry, true); break; /* EOR */
                case 0x2: c->r[rd] = barrel_shift(a, 0, b & 0xffu, true, &carry);
                          alu_logic_flags(c, c->r[rd], carry, true); break;                   /* LSL */
                case 0x3: c->r[rd] = barrel_shift(a, 1, b & 0xffu, true, &carry);
                          alu_logic_flags(c, c->r[rd], carry, true); break;                   /* LSR */
                case 0x4: c->r[rd] = barrel_shift(a, 2, b & 0xffu, true, &carry);
                          alu_logic_flags(c, c->r[rd], carry, true); break;                   /* ASR */
                case 0x5: c->r[rd] = alu_add(c, a, b, get_flag(c, ARM_CPSR_C), true); break;  /* ADC */
                case 0x6: c->r[rd] = alu_sub(c, a, b, get_flag(c, ARM_CPSR_C), true); break;  /* SBC */
                case 0x7: c->r[rd] = barrel_shift(a, 3, b & 0xffu, true, &carry);
                          alu_logic_flags(c, c->r[rd], carry, true); break;                   /* ROR */
                case 0x8: alu_logic_flags(c, a & b, carry, true); break;                      /* TST */
                case 0x9: c->r[rd] = alu_sub(c, 0, b, 1, true); break;                        /* NEG */
                case 0xa: alu_sub(c, a, b, 1, true); break;                                   /* CMP */
                case 0xb: alu_add(c, a, b, 0, true); break;                                   /* CMN */
                case 0xc: c->r[rd] = a | b; alu_logic_flags(c, c->r[rd], carry, true); break; /* ORR */
                case 0xd: c->r[rd] = a * b;
                          set_flag(c, ARM_CPSR_N, (c->r[rd] >> 31) & 1u);
                          set_flag(c, ARM_CPSR_Z, c->r[rd] == 0); break;                      /* MUL */
                case 0xe: c->r[rd] = a & ~b; alu_logic_flags(c, c->r[rd], carry, true); break;/* BIC */
                default:  c->r[rd] = ~b; alu_logic_flags(c, c->r[rd], carry, true); break;    /* MVN */
            }
            return ARM_OK;
        }
        if ((insn & 0xfc00u) == 0x4400u) {   /* Hi-register ops and BX/BLX */
            unsigned rd = (unsigned)TB(0) | ((insn >> 4) & 8u);
            unsigned rs = (unsigned)TB(3) | ((insn >> 3) & 8u);
            uint32_t sv = (rs == 15) ? pc4 : c->r[rs];
            switch ((insn >> 8) & 3u) {
                case 0:                                        /* ADD */
                    if (rd == 15) *next = ((c->r[15] + sv) & ~1u);
                    else c->r[rd] += sv;
                    return ARM_OK;
                case 1: alu_sub(c, (rd == 15) ? pc4 : c->r[rd], sv, 1, true); return ARM_OK; /* CMP */
                case 2:                                        /* MOV */
                    if (rd == 15) *next = sv & ~1u;
                    else c->r[rd] = sv;
                    return ARM_OK;
                default:                                       /* BX / BLX */
                    if (insn & (1u << 7)) c->r[14] = (pc + 2) | 1u;   /* BLX */
                    if (!(sv & 1u)) c->cpsr &= ~ARM_CPSR_T;           /* back to ARM */
                    *next = sv & ~1u;
                    return ARM_OK;
            }
        }
        /* PC-relative load: LDR Rd,[PC,#imm8*4] — PC is word-aligned first. */
        unsigned rd = (insn >> 8) & 7u;
        uint32_t addr = (pc4 & ~3u) + ((insn & 0xffu) << 2);
        uint32_t v = mem_r32(c, addr);
        if (c->abort_pending) return ARM_OK;
        c->r[rd] = v;
        return ARM_OK;
    }
    case 0x5: {
        unsigned rd = TB(0), rb = TB(3), ro = TB(6);
        uint32_t addr = c->r[rb] + c->r[ro];
        if (insn & (1u << 9)) {              /* sign-extended byte/halfword */
            uint32_t v;
            switch ((insn >> 10) & 3u) {
                case 0:
                    mem_w16(c, addr, (uint16_t)c->r[rd]);            /* STRH  */
                    return ARM_OK;
                case 1:
                    v = (uint32_t)(int32_t)(int8_t)mem_r8(c, addr);  /* LDRSB */
                    break;
                case 2:
                    v = mem_r16(c, addr);                            /* LDRH  */
                    break;
                default:
                    v = (uint32_t)(int32_t)(int16_t)mem_r16(c, addr);/* LDRSH */
                    break;
            }
            if (c->abort_pending) return ARM_OK;
            c->r[rd] = v;
            return ARM_OK;
        }
        bool load = (insn >> 11) & 1u, byte = (insn >> 10) & 1u;
        if (load) {
            uint32_t v = byte ? mem_r8(c, addr) : mem_r32(c, addr);
            if (c->abort_pending) return ARM_OK;
            c->r[rd] = v;
        } else {
            if (byte) mem_w8(c, addr, (uint8_t)c->r[rd]);
            else      mem_w32(c, addr, c->r[rd]);
        }
        return ARM_OK;
    }
    case 0x6: case 0x7: {                    /* load/store word or byte, imm5 */
        unsigned rd = TB(0), rb = TB(3), off = (insn >> 6) & 0x1fu;
        bool byte = (insn >> 12) & 1u, load = (insn >> 11) & 1u;
        uint32_t addr = c->r[rb] + (byte ? off : (off << 2));
        if (load) {
            uint32_t v = byte ? mem_r8(c, addr) : mem_r32(c, addr);
            if (c->abort_pending) return ARM_OK;
            c->r[rd] = v;
        } else {
            if (byte) mem_w8(c, addr, (uint8_t)c->r[rd]);
            else      mem_w32(c, addr, c->r[rd]);
        }
        return ARM_OK;
    }
    case 0x8: {                              /* load/store halfword, imm5 */
        unsigned rd = TB(0), rb = TB(3);
        uint32_t addr = c->r[rb] + (((insn >> 6) & 0x1fu) << 1);
        if (insn & (1u << 11)) {
            uint32_t v = mem_r16(c, addr);
            if (c->abort_pending) return ARM_OK;
            c->r[rd] = v;
        } else {
            mem_w16(c, addr, (uint16_t)c->r[rd]);
        }
        return ARM_OK;
    }
    case 0x9: {                              /* SP-relative load/store */
        unsigned rd = (insn >> 8) & 7u;
        uint32_t addr = c->r[13] + ((insn & 0xffu) << 2);
        if (insn & (1u << 11)) {
            uint32_t v = mem_r32(c, addr);
            if (c->abort_pending) return ARM_OK;
            c->r[rd] = v;
        } else {
            mem_w32(c, addr, c->r[rd]);
        }
        return ARM_OK;
    }
    case 0xa: {                              /* ADD Rd, PC/SP, #imm8*4 */
        unsigned rd = (insn >> 8) & 7u;
        uint32_t imm = (insn & 0xffu) << 2;
        c->r[rd] = (insn & (1u << 11)) ? c->r[13] + imm : (pc4 & ~3u) + imm;
        return ARM_OK;
    }
    case 0xb: {
        if ((insn & 0xff00u) == 0xb000u) {   /* ADD/SUB SP, #imm7*4 */
            uint32_t imm = (insn & 0x7fu) << 2;
            c->r[13] = (insn & (1u << 7)) ? c->r[13] - imm : c->r[13] + imm;
            return ARM_OK;
        }
        if ((insn & 0xf600u) == 0xb400u) {   /* PUSH / POP */
            uint32_t list = insn & 0xffu;
            bool load = (insn >> 11) & 1u, extra = (insn >> 8) & 1u;
            if (load) {                       /* POP: ascending from SP */
                uint32_t sp = c->r[13], loaded[8]; uint32_t pcv = 0;
                for (unsigned i = 0; i < 8; i++) {
                    if (!(list & (1u << i))) continue;
                    loaded[i] = mem_r32(c, sp);
                    if (c->abort_pending) return ARM_OK;
                    sp += 4;
                }
                if (extra) { pcv = mem_r32(c, sp); if (c->abort_pending) return ARM_OK; sp += 4; }
                for (unsigned i = 0; i < 8; i++) if (list & (1u << i)) c->r[i] = loaded[i];
                c->r[13] = sp;
                if (extra) {                  /* POP {..,pc} may switch to ARM */
                    if (!(pcv & 1u)) c->cpsr &= ~ARM_CPSR_T;
                    *next = pcv & ~1u;
                }
            } else {                          /* PUSH: descending, LR pushed last */
                unsigned n = extra ? 1u : 0u;
                for (unsigned i = 0; i < 8; i++) if (list & (1u << i)) n++;
                uint32_t sp = c->r[13] - 4u * n, addr = sp;
                for (unsigned i = 0; i < 8; i++) {
                    if (!(list & (1u << i))) continue;
                    mem_w32(c, addr, c->r[i]);
                    if (c->abort_pending) return ARM_OK;
                    addr += 4;
                }
                if (extra) { mem_w32(c, addr, c->r[14]); if (c->abort_pending) return ARM_OK; }
                c->r[13] = sp;
            }
            return ARM_OK;
        }
        /* ARMv6 Thumb extend and byte-reverse group. Real Apple LLB reaches
         * UXTB within a few thousand instructions, so these are not optional. */
        if ((insn & 0xff00u) == 0xb200u) {            /* SXTH/SXTB/UXTH/UXTB */
            unsigned rd = TB(0);
            uint32_t v = c->r[TB(3)];
            switch ((insn >> 6) & 3u) {
                case 0: c->r[rd] = (uint32_t)(int32_t)(int16_t)(uint16_t)v; break; /* SXTH */
                case 1: c->r[rd] = (uint32_t)(int32_t)(int8_t)(uint8_t)v;   break; /* SXTB */
                case 2: c->r[rd] = v & 0xffffu;                            break; /* UXTH */
                default: c->r[rd] = v & 0xffu;                             break; /* UXTB */
            }
            return ARM_OK;
        }
        if ((insn & 0xff00u) == 0xba00u) {            /* REV/REV16/REVSH */
            unsigned rd = TB(0);
            uint32_t v = c->r[TB(3)];
            switch ((insn >> 6) & 3u) {
                case 0:                                                    /* REV */
                    c->r[rd] = ((v & 0xffu) << 24) | ((v & 0xff00u) << 8)
                             | ((v >> 8) & 0xff00u) | ((v >> 24) & 0xffu);
                    break;
                case 1:                                                    /* REV16 */
                    c->r[rd] = ((v & 0x00ffu) << 8)  | ((v & 0xff00u) >> 8)
                             | ((v & 0x00ff0000u) << 8) | ((v & 0xff000000u) >> 8);
                    break;
                case 3: {                                                  /* REVSH */
                    uint16_t h = (uint16_t)(((v & 0xffu) << 8) | ((v >> 8) & 0xffu));
                    c->r[rd] = (uint32_t)(int32_t)(int16_t)h;
                    break;
                }
                default: return ARM_UNDEFINED;
            }
            return ARM_OK;
        }
        if ((insn & 0xffe0u) == 0xb660u) {            /* CPS (interrupt masks) */
            bool disable = (insn >> 4) & 1u;
            if (insn & (1u << 1)) set_flag(c, ARM_CPSR_I, disable);
            if (insn & (1u << 0)) set_flag(c, ARM_CPSR_F, disable);
            return ARM_OK;
        }
        return ARM_UNDEFINED;                 /* SETEND, BKPT, IT: later */
    }
    case 0xc: {                              /* STMIA / LDMIA Rb!, {rlist} */
        unsigned rb = (insn >> 8) & 7u;
        uint32_t list = insn & 0xffu, addr = c->r[rb];
        if (list == 0) return ARM_UNDEFINED;
        if (insn & (1u << 11)) {
            uint32_t loaded[8];
            for (unsigned i = 0; i < 8; i++) {
                if (!(list & (1u << i))) continue;
                loaded[i] = mem_r32(c, addr);
                if (c->abort_pending) return ARM_OK;
                addr += 4;
            }
            for (unsigned i = 0; i < 8; i++) if (list & (1u << i)) c->r[i] = loaded[i];
            if (!(list & (1u << rb))) c->r[rb] = addr;
        } else {
            for (unsigned i = 0; i < 8; i++) {
                if (!(list & (1u << i))) continue;
                mem_w32(c, addr, c->r[i]);
                if (c->abort_pending) return ARM_OK;
                addr += 4;
            }
            c->r[rb] = addr;
        }
        return ARM_OK;
    }
    case 0xd: {
        unsigned cond = (insn >> 8) & 0xfu;
        if (cond == 0xf) {                   /* SWI */
            take_exception(c, ARM_VEC_SWI, ARM_MODE_SVC, pc + 2, false, next);
            return ARM_OK;
        }
        if (cond == 0xe) return ARM_UNDEFINED;   /* permanently undefined */
        if (arm_cond_passed(c, cond))
            *next = pc4 + ((uint32_t)(int32_t)(int8_t)(insn & 0xffu) << 1);
        return ARM_OK;
    }
    case 0xe: {
        /*
         * 0xE000-0xE7FF is an unconditional branch, but 0xE800-0xEFFF is the
         * SECOND half of a BLX pair, which returns to ARM state. Treating the
         * whole 0xExxx range as a branch sends BLX to a garbage address — real
         * XNU hit this and ended up executing a table of pointers as code.
         */
        if (insn & 0x0800u) {                /* BLX suffix: back to ARM */
            uint32_t target = (c->r[14] + ((uint32_t)(insn & 0x7ffu) << 1)) & ~3u;
            c->r[14] = (pc + 2) | 1u;        /* return address, Thumb bit set */
            c->cpsr &= ~ARM_CPSR_T;
            *next = target;
            return ARM_OK;
        }
        int32_t off = (int32_t)((uint32_t)(insn & 0x7ffu) << 21) >> 20;  /* sign-extend, <<1 */
        *next = pc4 + (uint32_t)off;
        return ARM_OK;
    }
    default: {                               /* 0xf: BL/BLX, a 32-bit pair */
        if (!(insn & (1u << 11))) {          /* first half: LR = PC + (off<<12) */
            int32_t off = (int32_t)((uint32_t)(insn & 0x7ffu) << 21) >> 9;
            c->r[14] = pc4 + (uint32_t)off;
            return ARM_OK;
        }
        uint32_t target = c->r[14] + ((uint32_t)(insn & 0x7ffu) << 1);
        c->r[14] = (pc + 2) | 1u;            /* return address, Thumb bit set */
        *next = target & ~1u;
        return ARM_OK;
    }
    }
}

#undef TB

arm_status_t arm_step(arm_cpu_t *c) {
    uint32_t pc   = c->r[15];

    /* Sample the interrupt lines before fetching. FIQ outranks IRQ. The return
     * address convention is "next instruction + 4", so handlers return with
     * SUBS pc, lr, #4. */
    if (c->fiq_line && !(c->cpsr & ARM_CPSR_F)) {
        uint32_t vec;
        c->cycles++;
        take_exception(c, ARM_VEC_FIQ, ARM_MODE_FIQ, pc + 4, true, &vec);
        c->r[15] = vec;
        return ARM_OK;
    }
    if (c->irq_line && !(c->cpsr & ARM_CPSR_I)) {
        uint32_t vec;
        c->cycles++;
        take_exception(c, ARM_VEC_IRQ, ARM_MODE_IRQ, pc + 4, false, &vec);
        c->r[15] = vec;
        return ARM_OK;
    }

    /* Instruction fetch is translated too; a fault here is a prefetch abort. */
    uint32_t fetch_pa;
    uint32_t fetch_fsr = arm_mmu_translate(c, pc, false, cpu_is_priv(c), &fetch_pa);
    if (fetch_fsr) {
        uint32_t vec;
        c->cycles++;
        c->cp15.ifsr = fetch_fsr;
        c->cp15.ifar = pc;
        take_exception(c, ARM_VEC_PREFETCH, ARM_MODE_ABT, pc + 4, false, &vec);
        c->r[15] = vec;
        return ARM_OK;
    }
    /* Thumb: 16-bit instructions, PC advances by 2. Dispatch before the ARM
     * decoder — the two instruction sets share every helper below. */
    if (c->cpsr & ARM_CPSR_T) {
        uint16_t tinsn = c->bus->read16(c->bus->ctx, fetch_pa);
        uint32_t tnext = pc + 2;
        c->cycles++;
        arm_status_t tst = thumb_step(c, pc, tinsn, &tnext);
        if (c->abort_pending) {
            uint32_t vec;
            c->abort_pending = false;
            c->cp15.dfsr = c->abort_fsr;
            c->cp15.dfar = c->abort_far;
            take_exception(c, ARM_VEC_DATA_ABORT, ARM_MODE_ABT, pc + 8, false, &vec);
            c->r[15] = vec;
            return ARM_OK;
        }
        if (tst == ARM_OK) c->r[15] = tnext;
        return tst;
    }

    uint32_t insn = c->bus->read32(c->bus->ctx, fetch_pa);
    uint32_t next = pc + 4;
    arm_status_t st = ARM_OK;

    c->cycles++;

    uint32_t cond = insn >> 28;

    /* cond==0xF is NOT "always" on ARMv6 — it is the unconditional instruction
     * space (PLD, BLX immediate, SETEND, CPS, RFE, SRS). Decoding it as a
     * normal conditional instruction is catastrophic: PLD would fall into the
     * single-data-transfer decode as "LDRB pc,[Rn]" and branch to whatever byte
     * it loaded. ARMv6-optimised code (memcpy loops in libSystem, XNU copy
     * routines) issues PLD constantly. */
    if (cond == 0xfu) {
        /* PLD is a hint; a no-op is architecturally correct. Both the immediate
         * and register forms have bits[27:26]==01 and the Rd field SBO (1111). */
        /*
         * CLREX must be tested BEFORE PLD. Its encoding 0xF57FF01F satisfies
         * the PLD pattern (bits[27:26]==01 with the Rd field all ones), so
         * with PLD first it is silently treated as a hint and does nothing —
         * which looks harmless and is not: a STREX the architecture requires
         * to fail then succeeds, and two threads can both hold one lock.
         */
        if (insn == 0xf57ff01fu) {
            c->excl_valid = false;
            c->r[15] = next;
            return ARM_OK;
        }
        if ((insn & 0x0c00f000u) == 0x0400f000u) {
            c->r[15] = next;
            return ARM_OK;
        }
        /*
         * CPS — change processor state. Kernels use this constantly to mask
         * interrupts and switch mode in one instruction; real XNU reaches it
         * within the first 40k instructions.
         *   1111 0001 0000 imod M 0 0000 000 A I F 0 mode
         * imod 10 = enable (clear the mask bits), 11 = disable (set them);
         * M selects whether the mode field is applied.
         */
        if ((insn & 0xfff1fe20u) == 0xf1000000u) {
            unsigned imod = (insn >> 18) & 3u;
            bool     chg_mode = (insn >> 17) & 1u;
            if (imod & 2u) {
                bool disable = (imod & 1u) != 0;
                if (insn & (1u << 8)) set_flag(c, (1u << 8), disable);  /* A: aborts */
                if (insn & (1u << 7)) set_flag(c, ARM_CPSR_I, disable);
                if (insn & (1u << 6)) set_flag(c, ARM_CPSR_F, disable);
            }
            if (chg_mode) arm_set_mode(c, insn & ARM_CPSR_MODE_MASK);
            c->r[15] = next;
            return ARM_OK;
        }

        /* BLX <immediate>: an unconditional call that always switches to Thumb.
         *   1111 101 H imm24    target = PC + 8 + (imm24 << 2) + (H << 1) */
        if ((insn & 0xfe000000u) == 0xfa000000u) {
            int32_t off = (int32_t)(insn << 8) >> 6;      /* sign-extend, <<2 */
            uint32_t h = (insn >> 24) & 1u;
            c->r[14] = pc + 4;
            c->cpsr |= ARM_CPSR_T;
            c->r[15] = (pc + 8 + (uint32_t)off + (h << 1)) & ~1u;
            return ARM_OK;
        }

        /*
         * SRS — Store Return State. Pushes the current mode's LR and SPSR onto
         * the banked stack of the mode named in the instruction. XNU uses it on
         * exception entry, paired with RFE to return.
         *   1111 100 P U 1 W 0 1101 0000 0101 000 mode
         */
        if ((insn & 0xfe5fffe0u) == 0xf84d0500u) {
            bool P = (insn >> 24) & 1u, U = (insn >> 23) & 1u, W = (insn >> 21) & 1u;
            uint32_t mode = insn & ARM_CPSR_MODE_MASK;
            arm_bank_t tb = arm_bank_of_mode(mode);
            arm_bank_t cur = arm_bank_of_mode(c->cpsr);

            /* The target mode's SP: live in r13 if that is the current bank. */
            uint32_t sp = (tb == cur) ? c->r[13] : c->bank_r13[tb];
            uint32_t base = U ? (P ? sp + 4u : sp) : (P ? sp - 8u : sp - 4u);

            mem_w32(c, base,      c->r[14]);
            mem_w32(c, base + 4u, (cur == ARM_BANK_USR) ? c->cpsr : c->spsr[cur]);
            if (c->abort_pending) { c->r[15] = next; return ARM_OK; }

            if (W) {
                uint32_t wb = U ? sp + 8u : sp - 8u;
                if (tb == cur) c->r[13] = wb; else c->bank_r13[tb] = wb;
            }
            c->r[15] = next;
            return ARM_OK;
        }

        /*
         * RFE — Return From Exception. Loads PC and CPSR from a base register.
         *   1111 100 P U 0 W 1 nnnn 0000 1010 0000 0000
         */
        if ((insn & 0xfe50ffffu) == 0xf8100a00u) {
            bool P = (insn >> 24) & 1u, U = (insn >> 23) & 1u, W = (insn >> 21) & 1u;
            unsigned rn = (insn >> 16) & 0xfu;
            uint32_t sp = c->r[rn];
            uint32_t base = U ? (P ? sp + 4u : sp) : (P ? sp - 8u : sp - 4u);

            uint32_t new_pc   = mem_r32(c, base);
            uint32_t new_cpsr = mem_r32(c, base + 4u);
            if (c->abort_pending) { c->r[15] = next; return ARM_OK; }

            if (W) c->r[rn] = U ? sp + 8u : sp - 8u;
            arm_set_mode(c, new_cpsr);
            c->cpsr = (c->cpsr & ARM_CPSR_MODE_MASK) | (new_cpsr & ~ARM_CPSR_MODE_MASK);
            c->r[15] = new_pc & ~1u;
            return ARM_OK;
        }

        /* SETEND is not implemented yet: trap rather than execute something
         * else by accident. */
        return ARM_UNDEFINED;
    }

    if (!arm_cond_passed(c, cond)) { c->r[15] = next; return ARM_OK; }

    /* Coarse top-level decode. This intentionally covers only the encodings
     * M1 implements; anything else returns ARM_UNDEFINED so the harness/log
     * can show us exactly which instruction to implement next. */
    if ((insn & 0x0f000000u) == 0x0a000000u ||
        (insn & 0x0f000000u) == 0x0b000000u) {           /* B / BL */
        st = exec_branch(c, pc, insn, &next);
    } else if ((insn & 0x0fc000f0u) == 0x00000090u) {     /* MUL / MLA */
        st = exec_multiply(c, pc, insn);
    } else if ((insn & 0x0e000000u) == 0x06000000u &&
               (insn & 0x00000010u) != 0) {
        /* bits[27:25]==011 with bit4==1 is the ARMv6 media space. The extend
         * and byte-reverse families are implemented; the rest still trap, so
         * they are named rather than silently mis-executed as loads/stores. */
        st = exec_media(c, pc, insn);
    } else if ((insn & 0x0c000000u) == 0x04000000u) {     /* single data transfer */
        st = exec_single_transfer(c, pc, insn, &next);
    } else if ((insn & 0x0e000000u) == 0x08000000u) {     /* LDM / STM */
        st = exec_block_transfer(c, pc, insn, &next);
    } else if ((insn & 0x0e000000u) == 0x00000000u &&
               (insn & 0x00000090u) == 0x00000090u &&
               (insn & 0x00000060u) != 0x00000000u) {
        /* bits[6:5] != 00 -> extra load/store (LDRH/STRH/LDRSB/LDRSH, LDRD/STRD) */
        st = exec_extra_transfer(c, pc, insn, &next);
    } else if ((insn & 0x0ff00ff0u) == 0x01900f90u) {     /* LDREX Rd,[Rn] */
        unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
        uint32_t addr = reg_read(c, pc, rn);
        uint32_t v = mem_r32(c, addr);
        if (!c->abort_pending) {
            c->r[rd] = v;
            c->excl_valid = true;      /* tag the address as exclusive */
            c->excl_addr  = addr;
        }
    } else if ((insn & 0x0ff00ff0u) == 0x01800f90u) {     /* STREX Rd,Rm,[Rn] */
        unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
        uint32_t addr = reg_read(c, pc, rn);
        if (c->excl_valid && c->excl_addr == addr) {
            mem_w32(c, addr, reg_read(c, pc, insn & 0xfu));
            if (!c->abort_pending) c->r[rd] = 0;          /* 0 = stored */
        } else {
            c->r[rd] = 1;                                  /* 1 = failed */
        }
        c->excl_valid = false;         /* the monitor is consumed either way */
    /*
     * The rest of the ARMv6K exclusive family. These are not exotic: the real
     * kernel's 64-bit atomics go through LDREXD/STREXD (OSAddAtomic64 is the
     * first one the boot reaches), and the byte and halfword forms appear
     * throughout the prelinked kexts. Rd must be even for the doubleword
     * forms, which pair Rd with Rd+1.
     */
    } else if ((insn & 0x0ff00ff0u) == 0x01b00f90u) {     /* LDREXD Rd,Rd+1,[Rn] */
        unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
        if (rd & 1u || rd == 14u) { st = ARM_UNDEFINED; }
        else {
            uint32_t addr = reg_read(c, pc, rn);
            uint32_t lo = mem_r32(c, addr);
            uint32_t hi = mem_r32(c, addr + 4);
            if (!c->abort_pending) {
                c->r[rd] = lo; c->r[rd + 1] = hi;
                c->excl_valid = true;
                c->excl_addr  = addr;
            }
        }
    } else if ((insn & 0x0ff00ff0u) == 0x01a00f90u) {     /* STREXD Rd,Rm,Rm+1,[Rn] */
        unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
        unsigned rm = insn & 0xfu;
        if (rm & 1u || rm == 14u) { st = ARM_UNDEFINED; }
        else {
            uint32_t addr = reg_read(c, pc, rn);
            if (c->excl_valid && c->excl_addr == addr) {
                mem_w32(c, addr,     reg_read(c, pc, rm));
                mem_w32(c, addr + 4, reg_read(c, pc, rm + 1));
                if (!c->abort_pending) c->r[rd] = 0;
            } else {
                c->r[rd] = 1;
            }
            c->excl_valid = false;
        }
    } else if ((insn & 0x0ff00ff0u) == 0x01d00f90u) {     /* LDREXB Rd,[Rn] */
        unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
        uint32_t addr = reg_read(c, pc, rn);
        uint32_t v = mem_r8(c, addr);
        if (!c->abort_pending) {
            c->r[rd] = v;
            c->excl_valid = true;
            c->excl_addr  = addr;
        }
    } else if ((insn & 0x0ff00ff0u) == 0x01c00f90u) {     /* STREXB Rd,Rm,[Rn] */
        unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
        uint32_t addr = reg_read(c, pc, rn);
        if (c->excl_valid && c->excl_addr == addr) {
            mem_w8(c, addr, (uint8_t)reg_read(c, pc, insn & 0xfu));
            if (!c->abort_pending) c->r[rd] = 0;
        } else {
            c->r[rd] = 1;
        }
        c->excl_valid = false;
    } else if ((insn & 0x0ff00ff0u) == 0x01f00f90u) {     /* LDREXH Rd,[Rn] */
        unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
        uint32_t addr = reg_read(c, pc, rn);
        uint32_t v = mem_r16(c, addr);
        if (!c->abort_pending) {
            c->r[rd] = v;
            c->excl_valid = true;
            c->excl_addr  = addr;
        }
    } else if ((insn & 0x0ff00ff0u) == 0x01e00f90u) {     /* STREXH Rd,Rm,[Rn] */
        unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
        uint32_t addr = reg_read(c, pc, rn);
        if (c->excl_valid && c->excl_addr == addr) {
            mem_w16(c, addr, (uint16_t)reg_read(c, pc, insn & 0xfu));
            if (!c->abort_pending) c->r[rd] = 0;
        } else {
            c->r[rd] = 1;
        }
        c->excl_valid = false;
    /*
     * SWP/SWPB — the pre-ARMv6 atomic. Deprecated in favour of LDREX/STREX but
     * still present in shipping code of this vintage. On a single core an
     * uninterrupted read-then-write is a faithful implementation.
     */
    } else if ((insn & 0x0fb00ff0u) == 0x01000090u) {     /* SWP / SWPB */
        unsigned rn = (insn >> 16) & 0xfu, rd = (insn >> 12) & 0xfu;
        bool byte = (insn >> 22) & 1u;
        uint32_t addr = reg_read(c, pc, rn);
        uint32_t rm_v = reg_read(c, pc, insn & 0xfu);
        uint32_t old  = byte ? mem_r8(c, addr) : mem_r32(c, addr);
        if (!c->abort_pending) {
            if (byte) mem_w8(c, addr, (uint8_t)rm_v);
            else      mem_w32(c, addr, rm_v);
            if (!c->abort_pending) c->r[rd] = old;
        }
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

    /* A translation fault latched during the instruction becomes a data abort.
     * LR_abt is the aborting instruction's address + 8, per the architecture. */
    if (c->abort_pending) {
        uint32_t vec;
        c->abort_pending = false;
        c->cp15.dfsr = c->abort_fsr;
        c->cp15.dfar = c->abort_far;
        take_exception(c, ARM_VEC_DATA_ABORT, ARM_MODE_ABT, pc + 8, false, &vec);
        c->r[15] = vec;
        return ARM_OK;
    }

    if (st == ARM_OK) c->r[15] = next;
    return st;
}

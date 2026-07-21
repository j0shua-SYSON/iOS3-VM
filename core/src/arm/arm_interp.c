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
#include "vfp.h"

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

/* The VFP unit reaches memory through the interpreter's own translating
 * accessors, so a VLDM that crosses into an unmapped page latches the fault
 * exactly like an LDM would and arm_step converts it to a data abort. */
static const vfp_bus_t g_vfp_bus = { mem_r32, mem_w32 };

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
    /*
     * ARM ARM (ARMv6, DDI0100I) A2.6: entry to Prefetch Abort, Data Abort, IRQ
     * and FIQ additionally sets CPSR.A, masking imprecise aborts; Undefined and
     * SWI leave it alone. Nothing in this machine raises an imprecise abort, so
     * the bit masks nothing — but the guest reads it. Every SPSR XNU saves and
     * every CPSR it prints on a panic carries it, and a nested exception taken
     * inside an abort handler must see A already set in the SPSR it stacks.
     */
    if (vector == ARM_VEC_PREFETCH || vector == ARM_VEC_DATA_ABORT ||
        vector == ARM_VEC_IRQ      || vector == ARM_VEC_FIQ)
        c->cpsr |= ARM_CPSR_A;
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

/* ============================ the Undefined-instruction discrimination =====
 *
 * Everything this core cannot execute returns ARM_UNDEFINED, which stops the
 * machine and prints the encoding and PC. That loudness is the point: it names
 * the next instruction to implement instead of letting the guest run on
 * fabricated results.
 *
 * There is exactly one family of encodings where stopping is WRONG, because
 * the guest is architecturally supposed to see the Undefined exception and
 * handle it. XNU does not leave VFP on. It clears FPEXC.EN and enables VFP
 * lazily, per thread: the first VFP instruction a thread executes is MEANT to
 * be undefined, and the kernel's handler turns VFP on and re-runs it.
 *
 *   _fleh_undef  0xc0067ff8  MRS sp,spsr; TST sp,#0x20; SUBEQ lr,lr,#4;
 *                            SUBNE lr,lr,#2  -> BL _sleh_undef
 *   _sleh_undef  0xc006c184  reads SPSR.T from the saved state, then matches
 *                            the faulting word against six masks (below). On a
 *                            match it calls _get_vfp_enabled (0xc006994c,
 *                            "VMRS r0,FPEXC; AND r1,r0,#0x40000000"):
 *                              FPEXC.EN == 0 -> _vfp_trap -> _vfp_switch,
 *                                 which sets FPEXC.EN and returns WITHOUT
 *                                 advancing PC, so the instruction re-runs;
 *                              FPEXC.EN != 0 -> falls through to
 *                                 EXC_BAD_INSTRUCTION (SIGILL / panic).
 *
 * That second branch is what makes this safe. The kernel itself refuses to
 * treat a VFP encoding as a lazy-enable fault once VFP is already on. So the
 * rule below is not a guess and not a blanket "vector everything":
 *
 *   VFP/SIMD encoding AND VFP currently unavailable -> vector the guest.
 *       Positively identified: this is the lazy-enable path, and the only
 *       thing the guest can do with it is turn VFP on and retry.
 *   VFP/SIMD encoding AND VFP already enabled       -> ARM_UNDEFINED, halt.
 *       We would have to actually compute a floating-point result. Vectoring
 *       here would hand the guest an encoding its own handler classifies as
 *       an illegal instruction, i.e. we would convert "not implemented" into
 *       a SIGILL in launchd and lose the diagnostic entirely.
 *   Anything else                                   -> ARM_UNDEFINED, halt.
 *
 * Because the trap is one-shot per thread, this cannot loop: after the guest
 * enables VFP the retry lands in the second case and stops loudly, naming the
 * exact instruction. Deliberately NOT vectored, though _sleh_undef also
 * recognises them: the ARM/Thumb breakpoints 0xE7FFDEFE / 0xDEFE, which mean
 * the guest has trapped on purpose and we want to see where.
 */

/*
 * The six masks are copied bit for bit out of _sleh_undef's ARM-state branch at
 * 0xc006c368 (its literal pool at 0xc006c50c). Using the kernel's own test
 * rather than one derived from the ARM ARM is deliberate: the set we vector is
 * then, by construction, exactly the set the shipped handler claims it can
 * handle. Anything outside it reaches the guest's "illegal instruction" path,
 * so it must reach ours instead.
 *
 *   0xfe000000/0xf2000000  Advanced SIMD data processing
 *   0x0f000e10/0x0e000a00  VFP data processing            (CDP  cp10/cp11)
 *   0x0e000e00/0x0c000a00  VFP load/store                 (LDC/STC cp10/cp11)
 *   0xff100000/0xf4000000  Advanced SIMD element/structure load/store
 *   0x0f000e10/0x0e000a10  VFP 32-bit register transfer   (MCR/MRC cp10/cp11)
 *   0x0fe00e00/0x0c400a00  VFP 64-bit register transfer   (MCRR/MRRC cp10/11)
 *
 * There is no Thumb counterpart here. _sleh_undef has one (0xc006c420, masks
 * 0xef000e10/0xee000a00 and friends) because the same source builds for
 * ARMv7, but the ARM1176JZF-S is ARMv6 and has no Thumb-2: Thumb state on this
 * part cannot encode a coprocessor instruction at all, so no Thumb encoding
 * can ever be a lazy-VFP fault.
 */
static bool insn_is_vfp_space(uint32_t insn) {
    return (insn & 0xfe000000u) == 0xf2000000u
        || (insn & 0x0f000e10u) == 0x0e000a00u
        || (insn & 0x0e000e00u) == 0x0c000a00u
        || (insn & 0xff100000u) == 0xf4000000u
        || (insn & 0x0f000e10u) == 0x0e000a10u
        || (insn & 0x0fe00e00u) == 0x0c400a00u;
}

/* vfp_cpacr_permits() and vfp_enabled() live in vfp.c (declared in vfp.h):
 * they are half of the availability gate the VFP unit itself applies, and one
 * copy of that rule is the only safe number of copies.
 *
 * True only for the lazy-enable case described above. */
static bool vfp_lazy_enable_trap(const arm_cpu_t *c, uint32_t insn) {
    if (!insn_is_vfp_space(insn)) return false;
    return !vfp_cpacr_permits(c) || !vfp_enabled(c);
}

/*
 * Final disposition of an ARM-state encoding this core did not execute. Either
 * the guest gets its Undefined exception, or we stop.
 *
 * ARM ARM (ARMv6, DDI0100I) A2.6.7: R14_und is the address of the instruction
 * following the undefined one — PC+4 in ARM state, PC+2 in Thumb — the mode
 * becomes Undefined (0b11011), SPSR_und takes the old CPSR, CPSR.I is set and
 * CPSR.T cleared. CPSR.A is NOT set, unlike the abort and interrupt vectors;
 * take_exception already draws that distinction. The kernel confirms the
 * +4/+2 split from the other side: _fleh_undef recovers the faulting PC with
 * "SUBEQ lr,lr,#4 / SUBNE lr,lr,#2" keyed on SPSR.T alone.
 *
 * Only the ARM-state form exists here, and that is not an omission. The Thumb
 * return address would be PC+2, but no Thumb encoding can qualify: ARMv6 Thumb
 * has no coprocessor instructions, so a Thumb undefined instruction is never a
 * lazy-VFP fault and always stops the machine. Writing the +2 case would be
 * unreachable code standing in for a case this part cannot produce.
 */
static arm_status_t undefined_instruction(arm_cpu_t *c, uint32_t pc,
                                          uint32_t insn) {
    uint32_t vec;
    if (!vfp_lazy_enable_trap(c, insn)) return ARM_UNDEFINED;
    take_exception(c, ARM_VEC_UNDEFINED, ARM_MODE_UND, pc + 4u, false, &vec);
    c->r[15] = vec;
    return ARM_OK;
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
     * CP10/CP11 are the VFP11 coprocessor; this is its MCR/MRC form (VMOV to
     * and from a core register, and VMRS/VMSR). The whole unit — availability,
     * the register file and every instruction — is core/src/arm/vfp.c.
     *
     * Note that ARM_UNDEFINED is not necessarily fatal on this path: arm_step
     * routes it through undefined_instruction(), which vectors the guest when
     * — and only when — the access is one VFP itself is currently refusing.
     */
    if (cp == 10 || cp == 11) return vfp_execute(c, pc, insn, &g_vfp_bus);

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
            case 0: {
                /* CRm selects which identity bank; opc2 the register in it. */
                static const uint32_t id_crm1[8] = {   /* feature registers  */
                    ARM1176_ID_PFR0,  ARM1176_ID_PFR1,
                    ARM1176_ID_DFR0,  ARM1176_ID_AFR0,
                    ARM1176_ID_MMFR0, ARM1176_ID_MMFR1,
                    ARM1176_ID_MMFR2, ARM1176_ID_MMFR3,
                };
                static const uint32_t id_crm2[8] = {   /* ISA attributes     */
                    ARM1176_ID_ISAR0, ARM1176_ID_ISAR1,
                    ARM1176_ID_ISAR2, ARM1176_ID_ISAR3,
                    ARM1176_ID_ISAR4, ARM1176_ID_ISAR5, 0, 0,
                };
                if (crm == 0) {
                    if (opc2 == 0) v = ARM1176_MIDR;
                    else if (opc2 == 1) v = ARM1176_CACHE_TYPE;
                } else if (crm == 1) {
                    v = id_crm1[opc2];
                } else if (crm == 2) {
                    v = id_crm2[opc2];
                }
                break;
            }
            case 1:
                if (opc2 == 0) v = p->sctlr; else if (opc2 == 1) v = p->actlr;
                else if (opc2 == 2) v = p->cpacr;
                break;
            case 2:
                if (crm == 0) {
                    if (opc2 == 0) v = p->ttbr0; else if (opc2 == 1) v = p->ttbr1;
                    else if (opc2 == 2) v = p->ttbcr;
                }
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
                /* TTBCR[31:3] are SBZ/UNP on the ARM1176 and do not read back;
                 * mask on write so an MRC matches hardware. The MMU walker
                 * masks again at point of use, so translation is unaffected. */
                if (crm == 0) {
                    if (opc2 == 0) p->ttbr0 = v; else if (opc2 == 1) p->ttbr1 = v;
                    else if (opc2 == 2) p->ttbcr = v & 7u;
                }
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
    for (int i = 0; i < 32; i++) cpu->vfp_s[i] = 0;
    /* On reset the ARM1176 enters SVC mode with IRQ, FIQ and imprecise aborts
     * disabled and begins execution from the reset vector (0x0, or 0xffff0000
     * with high vectors). We start at 0x0; the machine layer relocates PC as
     * needed. */
    cpu->cpsr   = ARM_MODE_SVC | ARM_CPSR_I | ARM_CPSR_F | ARM_CPSR_A;
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

        /*
         * CLZ Rd, Rm — count leading zeros.
         *   cccc 0001 0110 1111 dddd 1111 0001 mmmm
         * Compilers emit this constantly for bit scans, allocators and
         * normalisation, so it is ordinary code rather than a DSP curiosity.
         */
        if ((insn & 0x0fff0ff0u) == 0x016f0f10u) {
            uint32_t v = reg_read(c, pc, insn & 0xfu);
            unsigned n = 0;
            if (!v) n = 32;
            else while (!(v & 0x80000000u)) { v <<= 1; n++; }
            c->r[(insn >> 12) & 0xfu] = n;
            return ARM_OK;
        }

        /*
         * Saturating arithmetic: QADD, QSUB, QDADD, QDSUB.
         *   cccc 0001 0op0 nnnn dddd 0000 0101 mmmm
         * These saturate to INT32_MIN/INT32_MAX rather than wrapping, and set
         * the sticky Q flag (CPSR bit 27) on saturation. Wrapping instead
         * would be silently wrong only at the extremes.
         */
        if ((insn & 0x0f900ff0u) == 0x01000050u) {
            unsigned op = (insn >> 21) & 3u;
            int32_t  m  = (int32_t)reg_read(c, pc, insn & 0xfu);
            int32_t  n  = (int32_t)reg_read(c, pc, (insn >> 16) & 0xfu);
            bool sat = false;
            int64_t acc;

            if (op == 2 || op == 3) {          /* QDADD / QDSUB: double Rn first */
                int64_t d = (int64_t)n * 2;
                if (d > INT32_MAX) { d = INT32_MAX; sat = true; }
                if (d < INT32_MIN) { d = INT32_MIN; sat = true; }
                n = (int32_t)d;
            }
            if (op == 0 || op == 2) acc = (int64_t)m + (int64_t)n;   /* QADD/QDADD */
            else                    acc = (int64_t)m - (int64_t)n;   /* QSUB/QDSUB */

            if (acc > INT32_MAX) { acc = INT32_MAX; sat = true; }
            if (acc < INT32_MIN) { acc = INT32_MIN; sat = true; }

            c->r[(insn >> 12) & 0xfu] = (uint32_t)(int32_t)acc;
            if (sat) c->cpsr |= ARM_CPSR_Q;    /* sticky: only ever set here */
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
            if (i == 15 && !restore_cpsr) {
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
            /* The exception-return form (LDM {...,pc}^) is deliberately NOT
             * handled here: there, bit 0 of the loaded word is not a Thumb
             * selector — T comes from the SPSR, which has not been restored
             * yet. It is finished below, after the restore. */
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

        /* Now that T is back, finish the PC write for LDM {...,pc}^. The
         * restored T decides both the instruction set and the alignment; bit 0
         * of the loaded word is part of the address, not a selector. Doing
         * this before the restore — as the plain-LDM path above does — would
         * both interwork off the wrong bit and align for the wrong state. */
        if (L && (list & (1u << 15)))
            *next = loaded[15] & (uint32_t)((c->cpsr & ARM_CPSR_T) ? ~1u : ~3u);
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

/*
 * The 64-bit multiplies: UMULL, UMLAL, SMULL, SMLAL.
 *
 *   cccc 0000 1 Un A S hhhh llll ssss 1001 mmmm
 *
 * Bit 22 SELECTS SIGNED, not unsigned: 100=UMULL, 101=UMLAL, 110=SMULL,
 * 111=SMLAL for bits[23:21]. Getting that polarity backwards is invisible for
 * small operands and wrong only in the high word, which is exactly the sort of
 * error that shows up a long way from its cause. A accumulates into the existing
 * RdHi:RdLo. These are ordinary compiled code — a driver in the real 3.1.3
 * kernelcache stopped the boot dead on a plain UMULL — so the distinction
 * between signed and unsigned matters: doing the multiply in 64-bit unsigned
 * for both gives the right low word and a silently wrong high word.
 */
static arm_status_t exec_multiply_long(arm_cpu_t *c, uint32_t pc, uint32_t insn) {
    bool is_signed = (insn >> 22) & 1u;
    bool A = (insn >> 21) & 1u;
    bool S = (insn >> 20) & 1u;
    unsigned rdhi = (insn >> 16) & 0xfu;
    unsigned rdlo = (insn >> 12) & 0xfu;
    unsigned rs   = (insn >> 8)  & 0xfu;
    unsigned rm   = insn & 0xfu;

    /* PC as an operand or destination here is unpredictable, and RdHi must
     * differ from RdLo. Trap rather than invent a result. */
    if (rdhi == 15 || rdlo == 15 || rs == 15 || rm == 15 || rdhi == rdlo)
        return ARM_UNDEFINED;

    uint32_t a = reg_read(c, pc, rm), b = reg_read(c, pc, rs);
    uint64_t res;
    if (is_signed) {
        /* Sign-extend through int64 so the high word is right. */
        res = (uint64_t)((int64_t)(int32_t)a * (int64_t)(int32_t)b);
    } else {
        res = (uint64_t)a * (uint64_t)b;
    }
    if (A) res += ((uint64_t)c->r[rdhi] << 32) | (uint64_t)c->r[rdlo];

    c->r[rdlo] = (uint32_t)res;
    c->r[rdhi] = (uint32_t)(res >> 32);
    if (S) {
        set_flag(c, ARM_CPSR_N, (res >> 63) & 1u);
        set_flag(c, ARM_CPSR_Z, res == 0);
        /* C and V are unpredictable after a long multiply on ARMv6. */
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
        /* CPS: 1011 0110 011 im 0 A I F. Like the ARM form it is a no-op in
         * User mode — see the ARM encoding for why that matters. */
        if ((insn & 0xffe0u) == 0xb660u) {
            bool disable = (insn >> 4) & 1u;
            if (!cpu_is_priv(c)) return ARM_OK;
            if (insn & (1u << 2)) set_flag(c, ARM_CPSR_A, disable);
            if (insn & (1u << 1)) set_flag(c, ARM_CPSR_I, disable);
            if (insn & (1u << 0)) set_flag(c, ARM_CPSR_F, disable);
            return ARM_OK;
        }
        /* SETEND, Thumb encoding: 1011 0110 0101 0E00. Same reasoning as the
         * ARM form — LE is a genuine no-op for a little-endian machine, BE
         * keeps trapping because we do not model a big-endian data path and
         * honouring it would corrupt every subsequent load. */
        if ((insn & 0xfff7u) == 0xb650u) {
            if (insn & (1u << 3)) return ARM_UNDEFINED;   /* SETEND BE */
            return ARM_OK;
        }
        return ARM_UNDEFINED;                 /* BKPT and friends: still trap */
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
            /*
             * "CPS is a no-op if executed in User mode" (ARM ARM A4.1.16).
             * Unprivileged code can neither unmask an interrupt nor change
             * mode. Executing it regardless is a privilege escalation: a user
             * program running "CPS #0x13" would continue in SVC with its own
             * registers and its own PC, and "CPSIE i" would let it run with
             * interrupts it is not allowed to touch. The kernel-only boot can
             * never expose this — XNU only ever reaches CPS from a privileged
             * mode — so it stays invisible until launchd and dyld run.
             */
            if (!cpu_is_priv(c)) { c->r[15] = next; return ARM_OK; }
            unsigned imod = (insn >> 18) & 3u;
            bool     chg_mode = (insn >> 17) & 1u;
            if (imod & 2u) {
                bool disable = (imod & 1u) != 0;
                if (insn & ARM_CPSR_A) set_flag(c, ARM_CPSR_A, disable);
                if (insn & ARM_CPSR_I) set_flag(c, ARM_CPSR_I, disable);
                if (insn & ARM_CPSR_F) set_flag(c, ARM_CPSR_F, disable);
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
            /* Align for the state the restored CPSR selects, not always to a
             * halfword: returning into ARM code must land on a word boundary.
             * Same family of bug as the MOVS pc,lr path — see
             * exec_data_processing. */
            c->r[15] = new_pc & (uint32_t)((c->cpsr & ARM_CPSR_T) ? ~1u : ~3u);
            return ARM_OK;
        }

        /*
         * SETEND — select the data endianness for loads and stores.
         *   1111 0001 0000 0001 0000 0000 E000 0000
         * This core, the bus, and the guest are all little-endian, so SETEND LE
         * is genuinely a no-op and executing it is correct. SETEND BE is not:
         * we do not model a big-endian data path, so honouring it would mean
         * silently doing the wrong thing on every subsequent load. It keeps
         * trapping, which is the honest answer.
         */
        if ((insn & 0xfffffdffu) == 0xf1010000u) {
            if (insn & (1u << 9)) return ARM_UNDEFINED;   /* SETEND BE */
            c->r[15] = next;
            return ARM_OK;
        }
        /* The Advanced SIMD spaces (0xF2/0xF3 data processing, 0xF4 element
         * and structure load/store) live here, in the unconditional space, so
         * the lazy-VFP discrimination has to be applied on this arm too. */
        return undefined_instruction(c, pc, insn);
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
    } else if ((insn & 0x0f8000f0u) == 0x00800090u) {     /* UMULL/UMLAL/SMULL/SMLAL */
        st = exec_multiply_long(c, pc, insn);
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
    } else if ((insn & 0x0f000e10u) == 0x0e000a00u ||     /* VFP CDP          */
               (insn & 0x0e000e00u) == 0x0c000a00u) {     /* VFP LDC/STC/MCRR */
        /* The rest of the VFP11 encoding space. The MCR/MRC form came back
         * from exec_coprocessor above; these three groups have no CP15
         * counterpart and so reach the unit directly. */
        st = vfp_execute(c, pc, insn, &g_vfp_bus);
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

    /* Every ARM-state encoding we declined funnels through here, wherever in
     * the decode tree it was rejected — the CDP, LDC/STC and MCRR/MRRC forms
     * of VFP all fall out of the bottom of the tree, and MCR/MRC comes back
     * from exec_coprocessor. One choke point, one rule. */
    if (st == ARM_UNDEFINED) return undefined_instruction(c, pc, insn);

    if (st == ARM_OK) c->r[15] = next;
    return st;
}

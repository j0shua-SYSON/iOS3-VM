/*
 * iOS3-VM — ARMv6 (ARM1176JZF-S) CPU core, public interface.
 *
 * The ARM1176JZF-S is the application processor inside the Samsung S5L8900
 * (original iPhone / iPhone 3G / iPod touch 1G) that iPhone OS 1–3 ran on.
 * This core is written as portable C11 with zero platform dependencies so it
 * builds and unit-tests on Windows/Linux/macOS and drops unchanged into the
 * iOS app. Memory is reached through host callbacks (see arm_bus_t).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_ARM_H
#define IOS3VM_ARM_H

#include <stdint.h>
#include <stdbool.h>

/* CPSR condition-flag bit positions. */
#define ARM_CPSR_N (1u << 31) /* Negative */
#define ARM_CPSR_Z (1u << 30) /* Zero     */
#define ARM_CPSR_C (1u << 29) /* Carry    */
#define ARM_CPSR_V (1u << 28) /* Overflow */
/* Sticky saturation. Set by the saturating arithmetic, never cleared by it —
 * software clears it with MSR when it wants to test a span of operations. */
#define ARM_CPSR_Q (1u << 27)

/* CPSR control bits. */
/* A: imprecise (asynchronous) data-abort disable, new in ARMv6. Set on entry
 * to Reset, Prefetch Abort, Data Abort, IRQ and FIQ — see take_exception. We
 * model no asynchronous abort source, so nothing is actually masked by it; it
 * exists so the CPSR and every SPSR the guest saves hold the value hardware
 * would have put there. */
#define ARM_CPSR_A (1u << 8)  /* async abort disable */
#define ARM_CPSR_I (1u << 7)  /* IRQ disable  */
#define ARM_CPSR_F (1u << 6)  /* FIQ disable  */
#define ARM_CPSR_T (1u << 5)  /* Thumb state  */
#define ARM_CPSR_MODE_MASK 0x1fu

/* CP15 c1 system control register (SCTLR) bits we act on. */
#define ARM_SCTLR_M (1u << 0)   /* MMU enable          */
#define ARM_SCTLR_A (1u << 1)   /* alignment check     */
#define ARM_SCTLR_C (1u << 2)   /* data cache enable   */
#define ARM_SCTLR_I (1u << 12)  /* instruction cache   */
#define ARM_SCTLR_V (1u << 13)  /* high exception vectors @ 0xFFFF0000 */

/* Main ID register value reported for the ARM1176JZF-S in the S5L8900. */
#define ARM1176_MIDR       0x410fb767u
#define ARM1176_CACHE_TYPE 0x1d152152u
/* VFP11 identity register as reported by the ARM1176JZF-S. */
#define ARM1176_FPSID      0x410120b4u

/*
 * FPEXC, the VFP exception register. Only EN matters here: it is the switch
 * XNU flips to enable VFP for a thread, and with it clear every VFP
 * instruction except an access to FPEXC or FPSID is UNDEFINED. That is not a
 * quirk to work around — it is the mechanism the kernel relies on to enable
 * VFP lazily, one thread at a time. See vfp_lazy_enable_trap in arm_interp.c.
 */
#define ARM_FPEXC_EX (1u << 31)  /* exceptional state                        */
#define ARM_FPEXC_EN (1u << 30)  /* VFP enabled for the current thread       */

/*
 * CPACR, the coprocessor access control register, fields for CP10 and CP11 —
 * the two halves of the VFP/Advanced SIMD unit. Two bits each:
 *   00 access denied (any access is UNDEFINED)
 *   01 privileged access only (User-mode access is UNDEFINED)
 *   10 reserved
 *   11 full access
 * XNU's _init_vfp (0xc0069938) does "CPACR |= 0xf << 20", granting full
 * access to both, and then gates per thread with FPEXC.EN alone.
 */
#define ARM_CPACR_CP10_SHIFT 20
#define ARM_CPACR_CP11_SHIFT 22

/*
 * The ARMv6 CPUID feature identification block: CP15 c0 with CRm == 1
 * (processor / memory-model features) and CRm == 2 (instruction set
 * attributes). These are read-only and, on the ARM1176JZF-S, constant —
 * the values below are the part's, from ARM DDI 0301H section 3.2.
 *
 * They are not optional decoration. MIDR[19:16] on this core reads 0xF,
 * which by the ARM ARM means "architecture version is described by the
 * CPUID scheme, not by this field" — so any reader that wants the actual
 * architecture MUST come here. XNU does exactly that: do_cpuid() sees the
 * 0xF, reads ID_ISAR1, checks the Jazelle field, and only then rewrites
 * its cached MIDR to say ARMv6. Without ID_ISAR1 the kernel concludes the
 * CPU has no known architecture and reports CPU_SUBTYPE_ARM_ALL, which
 * makes grade_binary() reject every ARMv6 executable on the disk.
 */
#define ARM1176_ID_PFR0    0x00000111u  /* ARM + Thumb + Jazelle state     */
#define ARM1176_ID_PFR1    0x00000011u  /* programmers' model: v6, no TrustZone */
/*
 * ID_DFR0 is the one deliberate zero. The real part answers 0x00000033 —
 * "coprocessor-accessed debug, ARMv6.1" — but we do not implement the CP14
 * debug unit; DBGDIDR reads as zero here. XNU's do_debugid() takes a
 * non-zero ID_DFR0 as licence to publish a breakpoint/watchpoint count
 * derived from DBGDIDR, so claiming 0x33 would advertise debug hardware
 * that does not exist. Zero is the truthful answer for this machine, and
 * the architecture defines it to mean "no debug support".
 */
#define ARM1176_ID_DFR0    0x00000000u
#define ARM1176_ID_AFR0    0x00000000u  /* no implementation-defined features */
#define ARM1176_ID_MMFR0   0x01130003u
#define ARM1176_ID_MMFR1   0x10030302u
#define ARM1176_ID_MMFR2   0x01222100u
#define ARM1176_ID_MMFR3   0x00000000u
#define ARM1176_ID_ISAR0   0x00140011u
#define ARM1176_ID_ISAR1   0x12002111u  /* [15:12] = 2: Jazelle BXJ + J bit */
#define ARM1176_ID_ISAR2   0x11231121u
#define ARM1176_ID_ISAR3   0x01102131u
#define ARM1176_ID_ISAR4   0x00000141u
#define ARM1176_ID_ISAR5   0x00000000u

/* ARMv6 fault status codes (FSR[3:0]); FSR[7:4] carries the domain. */
#define ARM_FSR_SECTION_TRANSLATION 0x5u
#define ARM_FSR_PAGE_TRANSLATION    0x7u
#define ARM_FSR_SECTION_DOMAIN      0x9u
#define ARM_FSR_PAGE_DOMAIN         0xbu
#define ARM_FSR_SECTION_PERMISSION  0xdu
#define ARM_FSR_PAGE_PERMISSION     0xfu

/* Exception vector offsets (added to 0x0, or 0xFFFF0000 when SCTLR.V is set). */
#define ARM_VEC_RESET      0x00u
#define ARM_VEC_UNDEFINED  0x04u
#define ARM_VEC_SWI        0x08u
#define ARM_VEC_PREFETCH   0x0cu
#define ARM_VEC_DATA_ABORT 0x10u
#define ARM_VEC_IRQ        0x18u
#define ARM_VEC_FIQ        0x1cu

/* Processor modes (CPSR[4:0]). Only the ones we currently model are named. */
#define ARM_MODE_USR 0x10
#define ARM_MODE_FIQ 0x11
#define ARM_MODE_IRQ 0x12
#define ARM_MODE_SVC 0x13
#define ARM_MODE_ABT 0x17
#define ARM_MODE_UND 0x1b
#define ARM_MODE_SYS 0x1f

/* Result of stepping the core. */
typedef enum {
    ARM_OK = 0,          /* one instruction retired normally          */
    ARM_UNDEFINED,       /* undefined / not-yet-implemented encoding   */
    ARM_HALT             /* core requested halt (e.g. debug trap)      */
} arm_status_t;

/*
 * The system bus. The CPU is agnostic about what lives behind these; the test
 * harness supplies flat RAM, the machine layer supplies the S5L8900 memory map.
 * All accesses are little-endian, matching the guest.
 */
typedef struct arm_bus {
    void    *ctx;
    uint32_t (*read32)(void *ctx, uint32_t addr);
    uint16_t (*read16)(void *ctx, uint32_t addr);
    uint8_t  (*read8 )(void *ctx, uint32_t addr);
    void     (*write32)(void *ctx, uint32_t addr, uint32_t val);
    void     (*write16)(void *ctx, uint32_t addr, uint16_t val);
    void     (*write8 )(void *ctx, uint32_t addr, uint8_t  val);
} arm_bus_t;

/*
 * Register banks. ARM banks r13/r14 per privileged mode, and additionally
 * banks r8–r12 for FIQ. USR and SYS share one bank and have no SPSR.
 */
typedef enum {
    ARM_BANK_USR = 0, ARM_BANK_FIQ, ARM_BANK_IRQ,
    ARM_BANK_SVC, ARM_BANK_ABT, ARM_BANK_UND, ARM_BANK_COUNT
} arm_bank_t;

/*
 * CP15, the system control coprocessor. The kernel programs the MMU, caches,
 * and vector base through here, so these values steer real execution.
 */
typedef struct arm_cp15 {
    uint32_t sctlr;       /* c1,c0,0  system control            */
    uint32_t actlr;       /* c1,c0,1  auxiliary control         */
    uint32_t cpacr;       /* c1,c0,2  coprocessor access        */
    uint32_t ttbr0;       /* c2,c0,0  translation table base 0  */
    uint32_t ttbr1;       /* c2,c0,1  translation table base 1  */
    uint32_t ttbcr;       /* c2,c0,2  translation table control */
    uint32_t dacr;        /* c3,c0,0  domain access control     */
    uint32_t dfsr, ifsr;  /* c5       fault status              */
    uint32_t dfar, ifar;  /* c6       fault address             */
    uint32_t fcse_pid;    /* c13,c0,0 */
    uint32_t context_id;  /* c13,c0,1 */
    /* Software thread-ID registers. The kernel keeps per-CPU data in TPIDRPRW,
     * so these must actually store rather than read back as zero. */
    uint32_t tpidrurw;    /* c13,c0,2 user read/write */
    uint32_t tpidruro;    /* c13,c0,3 user read-only  */
    uint32_t tpidrprw;    /* c13,c0,4 privileged only */
} arm_cp15_t;

typedef struct arm_cpu {
    uint32_t r[16];      /* r0–r15; r15 is PC (address of current instruction) */
    uint32_t cpsr;
    arm_cp15_t cp15;
    uint32_t spsr[ARM_BANK_COUNT];     /* saved CPSR per privileged bank        */
    uint32_t bank_r13[ARM_BANK_COUNT]; /* banked stack pointers                 */
    uint32_t bank_r14[ARM_BANK_COUNT]; /* banked link registers                 */
    uint32_t fiq_r8_12[5];             /* FIQ's private r8–r12                  */
    uint32_t usr_r8_12[5];             /* user r8–r12 parked while in FIQ mode  */
    uint64_t cycles;     /* retired-instruction counter (1 insn == 1 tick for now) */
    const arm_bus_t *bus;

    /* A translation fault raised mid-instruction; arm_step turns this into a
     * data abort once the instruction finishes. */
    bool     abort_pending;
    uint32_t abort_fsr;
    uint32_t abort_far;

    /* Interrupt request lines, driven by the interrupt controller. Sampled at
     * the top of each arm_step and honoured unless masked by CPSR I/F. */
    bool irq_line;
    bool fiq_line;

    /* ARMv6 exclusive monitor for LDREX/STREX. Every XNU atomic and every
     * spinlock goes through these, so they are load-bearing rather than
     * optional. One CPU means a single address tag is sufficient. */
    bool     excl_valid;
    uint32_t excl_addr;

    /* VFP11 system registers. The kernel disables VFP and enables it lazily
     * per thread, so the context-switch path reads and writes FPEXC on every
     * switch. FPSID is a constant (ARM1176_FPSID) and so is not stored. */
    uint32_t vfp_fpexc;
    uint32_t vfp_fpscr;
    /*
     * The VFP register file: s0-s31 as raw bit patterns, in register-number
     * order. VFPv2 on the ARM1176 has 16 double-precision registers ALIASED
     * onto these, not a separate bank: dN is the pair (vfp_s[2N], vfp_s[2N+1])
     * with the LOW-order word first. Keeping one array and deriving dN from it
     * is what makes the aliasing impossible to get wrong — there is no second
     * copy that could drift. There is no d16-d31 on this part; see vfp.c.
     */
    uint32_t vfp_s[32];
} arm_cpu_t;

/*
 * Translate a virtual address. Returns 0 on success (writing the physical
 * address to *pa) or a non-zero ARMv6 fault status register value.
 * With the MMU disabled (SCTLR.M clear) translation is the identity map.
 */
uint32_t arm_mmu_translate(arm_cpu_t *cpu, uint32_t va, bool write, bool priv,
                           uint32_t *pa);

/* Which bank a CPSR mode value selects (USR and SYS share ARM_BANK_USR). */
arm_bank_t arm_bank_of_mode(uint32_t mode);

/* Switch processor mode, swapping the banked registers in and out. */
void arm_set_mode(arm_cpu_t *cpu, uint32_t mode);

/* Put the core into a defined post-reset state (SVC mode, IRQ/FIQ masked). */
void arm_reset(arm_cpu_t *cpu, const arm_bus_t *bus);

/* Fetch, decode, and execute exactly one instruction. */
arm_status_t arm_step(arm_cpu_t *cpu);

/* Evaluate an ARM 4-bit condition field against the current flags. */
bool arm_cond_passed(const arm_cpu_t *cpu, uint32_t cond);

#endif /* IOS3VM_ARM_H */

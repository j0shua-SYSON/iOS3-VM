/*
 * iOS3-VM — ARMv6 MMU: virtual-to-physical translation.
 *
 * Implements the ARMv6 short-descriptor translation scheme the ARM1176 uses:
 * a 4096-entry first-level table indexed by VA[31:20], whose entries are either
 * a 1 MB section or a pointer to a 256-entry second-level table indexed by
 * VA[19:12] holding 64 KB large pages or 4 KB small pages. Access is gated by
 * the domain field (via CP15 DACR) and the descriptor's AP bits.
 *
 * Table walks read guest memory through the same bus as everything else, so a
 * page table living in emulated RAM behaves exactly as it would on hardware.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "arm.h"

/*
 * Pack an ARMv6 fault status register value: status in [3:0], domain in [7:4],
 * and WnR in [11].
 *
 * The rest of the DFSR is deliberately zero, and each zero was checked against
 * what xnu-1357.5.30 actually reads. sleh_abort masks the saved DFSR with
 * 0x40f and tests bit 11 separately, so only three fields reach the kernel:
 *
 *   [3:0]  status  — we generate 5/7/9/b/d/f (section+page translation, domain
 *                    and permission). sleh_abort branches on every one of them.
 *   [10]   status[4], the extended fault codes (TLB conflict, lock abort).
 *                    Nothing in this machine can raise one, so it stays clear.
 *   [11]   WnR     — see below.
 *   [7:4]  domain  — read by nothing in the kernel, but cheap and correct.
 *   [12]   ExT     — external abort qualifier. We have no external abort source:
 *                    an unmapped bus read returns zero rather than signalling,
 *                    so status 0x8/0xc/0xe are unreachable and ExT stays clear.
 *   [1]/status 0b0001 — alignment. XNU clears SCTLR.A and sets SCTLR.U at
 *                    __start+0x16c, so the only alignment faults an ARM1176
 *                    could still raise are unaligned LDM/STM/LDRD/LDREX/SWP,
 *                    which this core does not detect. sleh_abort does route
 *                    (fsr & 0x40d) == 1 to sleh_alignment, so the kernel is
 *                    ready for one; we simply never produce it.
 *
 * DFSR[11] ("W: the abort was caused by a write access", DDI0100I) is not
 * optional bookkeeping. XNU's sleh_abort tests it to build the fault_type it
 * hands to arm_fast_fault; with the bit clear, every write fault is repaired as
 * if it were a read, so the PTE is rewritten with AP=0b10 (privileged RW, user
 * read-only) and an unprivileged store faults again on exactly the same
 * instruction, forever.
 *
 * This hid for ~230M instructions of boot because privileged writes are
 * accidentally satisfied by AP=0b10. Only an unprivileged access — STRT/LDRT,
 * or real user mode — can expose it, and the first one the kernel makes is the
 * copyout of "/sbin/launchd" into the pid-1 address space.
 *
 * IFSR has no WnR field, so the instruction-fetch path must pass write=false.
 * (It is fed from this same helper, so a domain also lands in IFSR[7:4]. The
 * ARM1176 documents that field as not meaningful for instruction faults;
 * fleh_prefabt hands the raw IFSR to sleh_abort, which masks it away with
 * 0x40f, so the kernel never sees it either way.)
 */
static inline uint32_t fsr_make(uint32_t status, unsigned domain, bool write) {
    return (status & 0xfu) | ((domain & 0xfu) << 4) | (write ? (1u << 11) : 0u);
}

/*
 * ARMv6 access permissions, including the APX (AP[2]) bit.
 *
 * APX:AP
 *   0:00  no access            0:01  privileged read/write only
 *   0:10  privileged RW, user read-only
 *   0:11  read/write for all
 *   1:00  reserved             1:01  privileged read-only
 *   1:10  read-only for all    1:11  read-only for all
 *
 * APX must not be ignored: with it dropped, every read-only mapping becomes
 * writable, so a guest kernel's copy-on-write faults and its read-only
 * kernel-text and page-table mappings would silently never fire.
 */
static bool ap_permits(unsigned ap, bool apx, bool write, bool priv) {
    if (apx) {
        switch (ap & 3u) {
            case 0:  return false;              /* reserved */
            case 1:  return priv && !write;     /* privileged read-only */
            default: return !write;             /* read-only for all */
        }
    }
    switch (ap & 3u) {
        case 0:  return false;
        case 1:  return priv;
        case 2:  return priv || !write;
        default: return true;
    }
}

uint32_t arm_mmu_translate(arm_cpu_t *c, uint32_t va, bool write, bool priv,
                           uint32_t *pa) {
    /* MMU disabled: physical == virtual. This is how every boot ROM starts. */
    if (!(c->cp15.sctlr & ARM_SCTLR_M)) { *pa = va; return 0; }

    /* --- first level ---------------------------------------------------------
     * ARMv6 TTBCR.N splits translation between TTBR0 and TTBR1. When the top N
     * bits of the VA are zero the walk starts from TTBR0 (whose table shrinks to
     * 2^(14-N) bytes and is indexed by VA[31-N:20]); otherwise it starts from
     * TTBR1, always a full 16 KB table indexed by VA[31:20].
     *
     * This is not optional bookkeeping. XNU-ARM on the ARM1176 runs with N=2, so
     * everything at and above 0x40000000 — all of kernel text and the 0xffff0000
     * exception-vector page — resolves through TTBR1, while TTBR0 holds the
     * *current user* pmap. Walking TTBR0 unconditionally happens to work only
     * while TTBR0 still maps kernel space; the first time the kernel loads a user
     * pmap into TTBR0 (pmap_switch calling set_mmu_ttb), kernel text and the
     * vector page disappear from the walk and the CPU storms forever on prefetch
     * aborts at 0xffff000c. Honouring N/TTBR1 is what keeps the kernel mapped
     * across that switch. */
    unsigned ttbcr_n = c->cp15.ttbcr & 7u;
    uint32_t l1_addr;
    if (ttbcr_n == 0u || (va >> (32u - ttbcr_n)) == 0u) {
        uint32_t base = c->cp15.ttbr0 & (0xffffffffu << (14u - ttbcr_n));
        uint32_t idx  = (va >> 20) & ((1u << (12u - ttbcr_n)) - 1u);
        l1_addr = base | (idx << 2);
    } else {
        l1_addr = (c->cp15.ttbr1 & 0xffffc000u) | ((va >> 20) << 2);
    }
    uint32_t l1      = c->bus->read32(c->bus->ctx, l1_addr);
    unsigned type    = l1 & 3u;
    unsigned domain  = (l1 >> 5) & 0xfu;

    if (type == 0) return fsr_make(ARM_FSR_SECTION_TRANSLATION, 0, write);

    unsigned dac = (c->cp15.dacr >> (domain * 2u)) & 3u;
    /* 00 = no access, 01 = client (check AP), 10 = reserved, 11 = manager. */
    if (dac == 0u || dac == 2u)
        return fsr_make(type == 2u ? ARM_FSR_SECTION_DOMAIN : ARM_FSR_PAGE_DOMAIN,
                        domain, write);

    if (type == 2u) {                       /* section or supersection */
        unsigned ap  = (l1 >> 10) & 3u;
        bool     apx = (l1 >> 15) & 1u;
        if (dac != 3u && !ap_permits(ap, apx, write, priv))
            return fsr_make(ARM_FSR_SECTION_PERMISSION, domain, write);

        /*
         * Bit 18 selects a 16 MB supersection, which takes its base from
         * bits[31:24] and the offset from va[23:0] — a different split from the
         * 1 MB section. Treating a supersection as a section silently resolves
         * the wrong physical address, which is exactly how a real XNU kernel
         * ended up reading garbage where a valid pointer lived.
         */
        if ((l1 >> 18) & 1u)
            *pa = (l1 & 0xff000000u) | (va & 0x00ffffffu);
        else
            *pa = (l1 & 0xfff00000u) | (va & 0x000fffffu);
        return 0;
    }

    if (type == 1u) {                       /* coarse second-level table */
        uint32_t l2_addr = (l1 & 0xfffffc00u) | (((va >> 12) & 0xffu) << 2);
        uint32_t l2      = c->bus->read32(c->bus->ctx, l2_addr);
        unsigned t2      = l2 & 3u;

        if (t2 == 0u) return fsr_make(ARM_FSR_PAGE_TRANSLATION, domain, write);

        unsigned ap  = (l2 >> 4) & 3u;
        bool     apx = (l2 >> 9) & 1u;
        if (dac != 3u && !ap_permits(ap, apx, write, priv))
            return fsr_make(ARM_FSR_PAGE_PERMISSION, domain, write);

        if (t2 == 1u) *pa = (l2 & 0xffff0000u) | (va & 0x0000ffffu); /* 64 KB */
        else          *pa = (l2 & 0xfffff000u) | (va & 0x00000fffu); /* 4 KB  */
        return 0;
    }

    /* type == 3 is the reserved first-level encoding; the ARM ARM defines it to
     * generate a section translation fault, which is what we return. (Sections
     * and supersections are both handled above, under type == 2.) */
    return fsr_make(ARM_FSR_SECTION_TRANSLATION, domain, write);
}

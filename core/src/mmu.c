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

/* Pack an ARMv6 fault status register value: status in [3:0], domain in [7:4]. */
static inline uint32_t fsr_make(uint32_t status, unsigned domain) {
    return (status & 0xfu) | ((domain & 0xfu) << 4);
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

    /* --- first level ---------------------------------------------------- */
    uint32_t l1_addr = (c->cp15.ttbr0 & 0xffffc000u) | ((va >> 20) << 2);
    uint32_t l1      = c->bus->read32(c->bus->ctx, l1_addr);
    unsigned type    = l1 & 3u;
    unsigned domain  = (l1 >> 5) & 0xfu;

    if (type == 0) return fsr_make(ARM_FSR_SECTION_TRANSLATION, 0);

    unsigned dac = (c->cp15.dacr >> (domain * 2u)) & 3u;
    /* 00 = no access, 01 = client (check AP), 10 = reserved, 11 = manager. */
    if (dac == 0u || dac == 2u)
        return fsr_make(type == 2u ? ARM_FSR_SECTION_DOMAIN : ARM_FSR_PAGE_DOMAIN,
                        domain);

    if (type == 2u) {                       /* 1 MB section */
        unsigned ap  = (l1 >> 10) & 3u;
        bool     apx = (l1 >> 15) & 1u;
        if (dac != 3u && !ap_permits(ap, apx, write, priv))
            return fsr_make(ARM_FSR_SECTION_PERMISSION, domain);
        *pa = (l1 & 0xfff00000u) | (va & 0x000fffffu);
        return 0;
    }

    if (type == 1u) {                       /* coarse second-level table */
        uint32_t l2_addr = (l1 & 0xfffffc00u) | (((va >> 12) & 0xffu) << 2);
        uint32_t l2      = c->bus->read32(c->bus->ctx, l2_addr);
        unsigned t2      = l2 & 3u;

        if (t2 == 0u) return fsr_make(ARM_FSR_PAGE_TRANSLATION, domain);

        unsigned ap  = (l2 >> 4) & 3u;
        bool     apx = (l2 >> 9) & 1u;
        if (dac != 3u && !ap_permits(ap, apx, write, priv))
            return fsr_make(ARM_FSR_PAGE_PERMISSION, domain);

        if (t2 == 1u) *pa = (l2 & 0xffff0000u) | (va & 0x0000ffffu); /* 64 KB */
        else          *pa = (l2 & 0xfffff000u) | (va & 0x00000fffu); /* 4 KB  */
        return 0;
    }

    /* Supersections and reserved encodings: not modelled yet — fault loudly. */
    return fsr_make(ARM_FSR_SECTION_TRANSLATION, domain);
}

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

typedef struct arm_cpu {
    uint32_t r[16];      /* r0–r15; r15 is PC (address of current instruction) */
    uint32_t cpsr;
    uint64_t cycles;     /* retired-instruction counter (1 insn == 1 tick for now) */
    const arm_bus_t *bus;
} arm_cpu_t;

/* Put the core into a defined post-reset state (SVC mode, IRQ/FIQ masked). */
void arm_reset(arm_cpu_t *cpu, const arm_bus_t *bus);

/* Fetch, decode, and execute exactly one instruction. */
arm_status_t arm_step(arm_cpu_t *cpu);

/* Evaluate an ARM 4-bit condition field against the current flags. */
bool arm_cond_passed(const arm_cpu_t *cpu, uint32_t cond);

#endif /* IOS3VM_ARM_H */

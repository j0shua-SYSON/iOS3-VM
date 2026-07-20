/*
 * iOS3-VM — runfw: execute a raw firmware payload on the emulated S5L8900.
 *
 * This is the diagnostic that turns "our tests pass" into "real Apple code
 * runs". It loads a decrypted payload, executes it, and — crucially — reports
 * exactly WHERE and WHY it stopped: the PC, the offending instruction, the
 * unmapped addresses it touched, and anything it printed over the UART.
 *
 * The failure output is the point. An unimplemented instruction or an
 * unmodelled peripheral is the next thing to build, and this tells us which.
 *
 * Usage:
 *   runfw <payload.bin> [-b <loadaddr>] [-n <steps>] [-v]
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *b = malloc((size_t)n ? (size_t)n : 1u);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f);
    *len_out = (size_t)n;
    return b;
}

static const char *status_name(arm_status_t s) {
    switch (s) {
        case ARM_OK:        return "OK";
        case ARM_UNDEFINED: return "UNDEFINED INSTRUCTION";
        case ARM_HALT:      return "HALT";
        default:            return "?";
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <payload.bin> [-b <loadaddr>] [-n <steps>] [-v]\n",
                argv[0]);
        return 1;
    }
    const char *path = argv[1];
    uint32_t base = 0x22000000u;      /* S5L8900 SRAM, where LLB runs */
    unsigned steps = 2000000u;
    bool verbose = false;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) base = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) steps = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-v")) verbose = true;
    }

    size_t len = 0;
    uint8_t *payload = slurp(path, &len);
    if (!payload) return 1;

    /* Give the guest a generous flat RAM covering the load address. */
    s5l8900_t m;
    if (!s5l8900_init(&m, base, 64u << 20)) { fprintf(stderr, "init failed\n"); return 1; }
    s5l8900_load(&m, base, payload, len);

    m.cpu.r[15] = base;
    printf("loaded %zu bytes at 0x%08x, running up to %u instructions\n\n",
           len, base, steps);

    arm_status_t st = ARM_OK;
    unsigned n = 0;
    uint32_t last_pc = base;
    /* Ring buffer of recent execution, so when the guest goes wrong we can see
     * the instructions that led there rather than guessing from static bytes. */
#define TRACE_N 24
    struct { uint32_t pc, cpsr; } trace[TRACE_N];
    unsigned tw = 0, tcount = 0;

    bool ran_away = false;
    for (; n < steps; n++) {
        last_pc = m.cpu.r[15];
        trace[tw].pc = last_pc;
        trace[tw].cpsr = m.cpu.cpsr;
        tw = (tw + 1) % TRACE_N;
        if (tcount < TRACE_N) tcount++;
        /* Executing outside the loaded image means a wrong branch was already
         * taken — almost always after reading a device we do not model and
         * getting zeros back. Stop here; running on buries the real cause. */
        if (last_pc < base || last_pc >= base + len) { ran_away = true; break; }
        st = arm_step(&m.cpu);
        if (st != ARM_OK) break;
        s5l8900_tick(&m, 1);
        if (verbose && n < 64)
            printf("  %6u  pc=%08x\n", n, last_pc);
    }
    if (ran_away)
        printf("*** PC left the loaded image: the guest branched into unmapped\n"
               "    memory. It almost certainly polled a device we do not model\n"
               "    and read back zeros.\n\n");

    m.uart0.tx[m.uart0.tx_len] = '\0';
    printf("stopped after %u instructions: %s\n", n, status_name(st));
    printf("  pc            : 0x%08x", last_pc);
    if (last_pc >= base && last_pc - base + 4 <= len) {
        uint32_t insn;
        memcpy(&insn, &payload[last_pc - base], 4);
        printf("  (offset 0x%x, instruction %08x)", last_pc - base, insn);
    }
    printf("\n");
    printf("  cpsr          : 0x%08x (mode %02x%s)\n", m.cpu.cpsr,
           m.cpu.cpsr & ARM_CPSR_MODE_MASK,
           (m.cpu.cpsr & ARM_CPSR_T) ? ", Thumb" : "");
    printf("  unmapped reads: %llu\n", (unsigned long long)m.unmapped_reads);
    printf("  unmapped writes:%llu\n", (unsigned long long)m.unmapped_writes);
    printf("  MMU           : %s\n",
           (m.cpu.cp15.sctlr & ARM_SCTLR_M) ? "enabled by the guest" : "off");
    if (m.cpu.cp15.ttbr0)
        printf("  TTBR0         : 0x%08x\n", m.cpu.cp15.ttbr0);

    printf("\n  registers:\n");
    for (int i = 0; i < 16; i += 4)
        printf("    r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n",
               i, m.cpu.r[i], i+1, m.cpu.r[i+1], i+2, m.cpu.r[i+2], i+3, m.cpu.r[i+3]);

    if (tcount) {
        printf("\n  last %u instructions executed:\n", tcount);
        unsigned start = (tw + TRACE_N - tcount) % TRACE_N;
        for (unsigned i = 0; i < tcount; i++) {
            unsigned k = (start + i) % TRACE_N;
            uint32_t pc = trace[k].pc;
            bool thumb = (trace[k].cpsr & ARM_CPSR_T) != 0;
            printf("    %08x  %s", pc, thumb ? "T " : "A ");
            if (pc >= base && pc - base + 4 <= len) {
                uint32_t w;
                memcpy(&w, &payload[pc - base], 4);
                if (thumb) printf("%04x", (unsigned)(w & 0xffff));
                else       printf("%08x", w);
            }
            printf("\n");
        }
    }

    if (m.unmapped_addr_count) {
        printf("\n  addresses touched outside the memory map (page-granular):\n");
        for (unsigned i = 0; i < m.unmapped_addr_count; i++)
            printf("    0x%08x\n", m.unmapped_addr[i]);
        printf("  ^ these name the peripherals still to be modelled.\n");
    }

    if (m.uart0.tx_len)
        printf("\n=== UART output (%zu bytes) ===\n%s\n", m.uart0.tx_len, m.uart0.tx);
    else
        printf("\n(no UART output)\n");

    s5l8900_free(&m);
    free(payload);
    return 0;
}

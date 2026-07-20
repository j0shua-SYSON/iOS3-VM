/*
 * iOS3-VM — bootkernel: load the real XNU kernelcache and start executing it.
 *
 * The kernel's segments carry VIRTUAL addresses (0xc0000000-based). At entry
 * the MMU is off, so each segment is placed at the physical address its virtual
 * address maps to, and execution begins at the physical entry point. XNU's own
 * early code then builds page tables and turns the MMU on.
 *
 *   phys = vmaddr - virt_base + phys_base
 *
 * ASSUMPTION, STATED: virt_base 0xc0000000 and phys_base = SDRAM base. The
 * virtual base is visible in the image itself (__HIB starts at 0xc0000000); the
 * physical base is our S5L8900 SDRAM address and is the thing most likely to be
 * wrong. Both are overridable so they can be probed rather than guessed at
 * silently.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "macho.h"
#include "soc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror("open"); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
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
        fprintf(stderr,
            "usage: %s <kernel.macho> [-p <physbase>] [-V <virtbase>] [-n <steps>]\n",
            argv[0]);
        return 1;
    }
    uint32_t phys_base = S5L8900_SDRAM_BASE;
    uint32_t virt_base = 0xc0000000u;
    unsigned steps = 2000000u;
    const char *dtpath = NULL;
    const char *cmdline = "debug=0x8 serial=1";
    bool stop_on_abort = false;

    /* Walk the arguments one at a time: pair-stepping breaks as soon as a
     * single-argument flag like -a appears. */
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) { stop_on_abort = true; continue; }
        if (i + 1 >= argc) break;
        if      (!strcmp(argv[i], "-p")) phys_base = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-V")) virt_base = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-n")) steps     = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-d")) dtpath    = argv[++i];
        else if (!strcmp(argv[i], "-c")) cmdline   = argv[++i];
    }

    size_t len = 0;
    uint8_t *img = slurp(argv[1], &len);
    if (!img) return 1;

    macho_t m;
    macho_status_t mst = macho_parse(img, len, &m);
    if (mst != MACHO_OK) { fprintf(stderr, "macho: %s\n", macho_strerror(mst)); return 2; }
    if (!m.has_entry)    { fprintf(stderr, "no entry point in the image\n"); return 2; }

    /* Enough RAM to hold the kernel plus room for its page tables and heap. */
    const uint32_t ram_size = 128u << 20;
    s5l8900_t mach;
    if (!s5l8900_init(&mach, phys_base, ram_size)) { fprintf(stderr, "init failed\n"); return 1; }
    mach.trace_devices = true;

    printf("kernel     : %s (%zu bytes)\n", argv[1], len);
    printf("virt base  : 0x%08x   phys base : 0x%08x   RAM %u MB\n",
           virt_base, phys_base, ram_size >> 20);

    for (unsigned i = 0; i < m.segment_count; i++) {
        macho_segment_t *s = &m.segments[i];
        if (!s->filesize) continue;
        if (s->vmaddr < virt_base) {
            printf("  skip %-16s vm 0x%08x is below the virtual base\n", s->name, s->vmaddr);
            continue;
        }
        uint32_t pa = s->vmaddr - virt_base + phys_base;
        s5l8900_load(&mach, pa, img + s->fileoff, s->filesize);
        printf("  load %-16s vm 0x%08x -> pa 0x%08x  %u bytes\n",
               s->name, s->vmaddr, pa, s->filesize);
    }

    uint32_t entry_pa = m.entry - virt_base + phys_base;
    printf("entry      : vm 0x%08x -> pa 0x%08x\n", m.entry, entry_pa);

    /*
     * Build boot_args. XNU derives where to put its page tables from these
     * fields, so passing zeros makes it compute nonsense addresses — which is
     * exactly what happened before this existed (TTBR0 came out as 0x18 and the
     * MMU walked unmapped memory).
     *
     * Layout below is the documented iOS ARM boot_args for this era. Revision
     * and Version are the values in common use; they are the fields I am least
     * sure of and the first thing to question if the kernel rejects the struct.
     */
    uint32_t dt_pa = (m.vm_high - virt_base + phys_base + 0xfffu) & ~0xfffu;
    uint32_t dt_len = 0;
    if (dtpath) {
        size_t n = 0;
        uint8_t *dt = slurp(dtpath, &n);
        if (dt) {
            dt_len = (uint32_t)n;
            s5l8900_load(&mach, dt_pa, dt, n);
            printf("devicetree : %s -> pa 0x%08x (%u bytes)\n", dtpath, dt_pa, dt_len);
            free(dt);
        }
    }

    uint32_t ba_pa = (dt_pa + dt_len + 0xfffu) & ~0xfffu;
    uint32_t top_of_kernel_data = (ba_pa + 0x1000u + 0xfffu) & ~0xfffu;

    uint8_t ba[0x138];
    memset(ba, 0, sizeof ba);
#define PUT16(o, v) do { ba[o] = (uint8_t)(v); ba[(o)+1] = (uint8_t)((v) >> 8); } while (0)
#define PUT32(o, v) do { ba[o] = (uint8_t)(v); ba[(o)+1] = (uint8_t)((v) >> 8); \
                         ba[(o)+2] = (uint8_t)((v) >> 16); ba[(o)+3] = (uint8_t)((v) >> 24); } while (0)
    PUT16(0x00, 1);                    /* Revision  */
    PUT16(0x02, 2);                    /* Version   */
    PUT32(0x04, virt_base);
    PUT32(0x08, phys_base);
    PUT32(0x0c, ram_size);
    /* PHYSICAL, not virtual: the kernel uses this value directly as the base
     * for its page tables. Passing the virtual form made TTBR0 come out as
     * 0xc07dc018 and the MMU walk unmapped memory. */
    PUT32(0x10, top_of_kernel_data);
    /* Boot_Video: no framebuffer yet, but give it a sane 320x480 geometry
     * rather than zeros, which some early code divides by. */
    PUT32(0x14, 0);                    /* v_baseAddr — none yet */
    PUT32(0x18, 0);                    /* v_display             */
    PUT32(0x1c, 320 * 4);              /* v_rowBytes            */
    PUT32(0x20, 320);                  /* v_width               */
    PUT32(0x24, 480);                  /* v_height              */
    PUT32(0x28, 32);                   /* v_depth               */
    PUT32(0x2c, 0);                    /* machineType           */
    PUT32(0x30, dt_len ? (dt_pa - phys_base + virt_base) : 0);
    PUT32(0x34, dt_len);
    memcpy(ba + 0x38, cmdline, strlen(cmdline) < 255 ? strlen(cmdline) : 255);
#undef PUT16
#undef PUT32
    s5l8900_load(&mach, ba_pa, ba, sizeof ba);
    printf("boot_args  : pa 0x%08x  topOfKernelData vm 0x%08x  cmdline \"%s\"\n\n",
           ba_pa, top_of_kernel_data - phys_base + virt_base, cmdline);

    mach.cpu.r[15] = entry_pa;
    mach.cpu.r[0]  = ba_pa;            /* XNU takes boot_args in r0 */

    /*
     * Ring buffer of recent execution. When the kernel faults we want the
     * instructions that led there, not the state a few million instructions
     * later once it is spinning in a handler.
     */
#define KTRACE 160
    struct { uint32_t pc, cpsr, r[16]; } tr[KTRACE];
    unsigned tw = 0, tcount = 0;

    arm_status_t st = ARM_OK;
    unsigned n = 0;
    uint32_t last_pc = entry_pa;
    unsigned first_abort_at = 0, first_exc = 0;
    uint32_t abort_dfar = 0, abort_dfsr = 0;

    for (; n < steps; n++) {
        last_pc = mach.cpu.r[15];
        tr[tw].pc = last_pc;
        tr[tw].cpsr = mach.cpu.cpsr;
        memcpy(tr[tw].r, mach.cpu.r, sizeof mach.cpu.r);
        tw = (tw + 1) % KTRACE;
        if (tcount < KTRACE) tcount++;

        uint32_t mode_before = mach.cpu.cpsr & ARM_CPSR_MODE_MASK;
        st = arm_step(&mach.cpu);
        if (st != ARM_OK) break;
        s5l8900_tick(&mach, 1);

        /* Report the first entry into an exception mode with the PC that caused
         * it — the kernel reading uninitialised per-CPU data usually means an
         * exception fired earlier than the kernel expected. */
        {
            uint32_t mode_after = mach.cpu.cpsr & ARM_CPSR_MODE_MASK;
            if (!first_exc && mode_after != mode_before &&
                (mode_after == ARM_MODE_ABT || mode_after == ARM_MODE_UND ||
                 mode_after == ARM_MODE_IRQ || mode_after == ARM_MODE_FIQ)) {
                first_exc = n;
                printf("FIRST exception entry at instruction %u: mode %02x -> %02x,\n"
                       "  caused by pc 0x%08x, vectored to 0x%08x\n"
                       "  (IFSR 0x%08x IFAR 0x%08x  DFSR 0x%08x DFAR 0x%08x)\n\n",
                       n, mode_before, mode_after, last_pc, mach.cpu.r[15],
                       mach.cpu.cp15.ifsr, mach.cpu.cp15.ifar,
                       mach.cpu.cp15.dfsr, mach.cpu.cp15.dfar);
            }
        }

        /* Catch the very first data abort and stop, so the trace above is the
         * code that actually faulted. */
        if (!first_abort_at && mach.cpu.cp15.dfsr) {
            first_abort_at = n;
            abort_dfsr = mach.cpu.cp15.dfsr;
            abort_dfar = mach.cpu.cp15.dfar;
            if (stop_on_abort) { n++; break; }
        }
    }

    if (first_abort_at) {
        printf("FIRST data abort at instruction %u: DFSR 0x%08x  DFAR 0x%08x\n\n",
               first_abort_at, abort_dfsr, abort_dfar);
        printf("  instructions leading up to it (newest last):\n");
        unsigned start = (tw + KTRACE - tcount) % KTRACE;
        for (unsigned i = 0; i < tcount; i++) {
            unsigned k = (start + i) % KTRACE;
            printf("    %08x  %s\n", tr[k].pc,
                   (tr[k].cpsr & ARM_CPSR_T) ? "Thumb" : "ARM");
        }
        unsigned last = (tw + KTRACE - 1) % KTRACE;
        printf("\n  registers at the faulting instruction:\n");
        for (int i = 0; i < 16; i += 4)
            printf("    r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n",
                   i, tr[last].r[i], i+1, tr[last].r[i+1],
                   i+2, tr[last].r[i+2], i+3, tr[last].r[i+3]);
        printf("\n");
    }

    mach.uart0.tx[mach.uart0.tx_len] = '\0';
    printf("stopped after %u instructions: %s\n", n, status_name(st));
    printf("  pc             : 0x%08x", last_pc);
    if (last_pc >= phys_base && last_pc < phys_base + ram_size)
        printf("  (vm 0x%08x)", last_pc - phys_base + virt_base);
    printf("\n");
    printf("  cpsr           : 0x%08x (mode %02x%s)\n", mach.cpu.cpsr,
           mach.cpu.cpsr & ARM_CPSR_MODE_MASK,
           (mach.cpu.cpsr & ARM_CPSR_T) ? ", Thumb" : "");
    printf("  MMU            : %s\n",
           (mach.cpu.cp15.sctlr & ARM_SCTLR_M) ? "ENABLED BY THE KERNEL" : "off");
    if (mach.cpu.cp15.ttbr0) printf("  TTBR0          : 0x%08x\n", mach.cpu.cp15.ttbr0);
    printf("  DFSR/DFAR      : 0x%08x / 0x%08x\n", mach.cpu.cp15.dfsr, mach.cpu.cp15.dfar);
    printf("  IFSR/IFAR      : 0x%08x / 0x%08x\n", mach.cpu.cp15.ifsr, mach.cpu.cp15.ifar);
    printf("  unmapped reads : %llu\n", (unsigned long long)mach.unmapped_reads);
    printf("  unmapped writes: %llu\n", (unsigned long long)mach.unmapped_writes);

    if (mach.unmapped_addr_count) {
        printf("\n  touched outside the memory map (page-granular):\n");
        for (unsigned i = 0; i < mach.unmapped_addr_count; i++)
            printf("    0x%08x\n", mach.unmapped_addr[i]);
    }

    if (mach.uart0.tx_len)
        printf("\n=== KERNEL UART OUTPUT (%zu bytes) ===\n%s\n",
               mach.uart0.tx_len, mach.uart0.tx);
    else
        printf("\n(no UART output yet)\n");

    s5l8900_free(&mach);
    free(img);
    return 0;
}

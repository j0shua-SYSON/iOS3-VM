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

    for (int i = 2; i + 1 < argc; i += 2) {
        if      (!strcmp(argv[i], "-p")) phys_base = (uint32_t)strtoul(argv[i+1], NULL, 0);
        else if (!strcmp(argv[i], "-V")) virt_base = (uint32_t)strtoul(argv[i+1], NULL, 0);
        else if (!strcmp(argv[i], "-n")) steps     = (unsigned)strtoul(argv[i+1], NULL, 0);
        else if (!strcmp(argv[i], "-d")) dtpath    = argv[i+1];
        else if (!strcmp(argv[i], "-c")) cmdline   = argv[i+1];
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

    arm_status_t st = ARM_OK;
    unsigned n = 0;
    uint32_t last_pc = entry_pa;
    for (; n < steps; n++) {
        last_pc = mach.cpu.r[15];
        st = arm_step(&mach.cpu);
        if (st != ARM_OK) break;
        s5l8900_tick(&mach, 1);
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

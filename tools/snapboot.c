/*
 * iOS3-VM — snapboot: the snapshot acceptance harness.
 *
 * WHY THIS EXISTS SEPARATELY FROM bootkernel. bootkernel is the debugging
 * instrument: it carries a 256 KB trace ring, a sampled profile, milestone hit
 * counts, a hot-page probe. All of that is HOST state accumulated since the
 * process started, so a run that restores a snapshot at instruction 200,000,000
 * legitimately prints different diagnostics from a run that reached the same
 * instruction the long way. Comparing those two logs would be comparing the
 * instrument, not the machine.
 *
 * snapboot performs the same machine setup as bootkernel — same kernel load,
 * same device-tree patches, same boot_args, same RAM-disk placement, same
 * kernel patch — and then prints ONLY things derived from the emulated machine.
 * It exists so the claim "a restored machine is the machine" can be tested
 * against the strongest possible evidence: two snapshot FILES, taken at the
 * same instruction count by two different processes, compared byte for byte.
 *
 *   snapboot kernel.macho -d dt.bin -c "..." -r rootfs.img -R 512 -n <steps>
 *            [--snapshot-at <steps> <file>]...   save at that instruction count
 *            [--restore <file>]                  start from a snapshot instead
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "macho.h"
#include "soc.h"
#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DTNAME 32

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

static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* --------------------------------------------------------------------------
 * Device-tree patching. This is bootkernel's logic, unchanged: iBoot fills in
 * the clock frequencies and the RAM-disk entry on real hardware, and the
 * shipped tree is a template full of zeros. The boot does not survive without
 * it, so the harness has to do the same job to reach the same machine.
 * ------------------------------------------------------------------------ */

static size_t dtn_hdr(const uint8_t *b, size_t len, size_t off,
                      uint32_t *np, uint32_t *nc) {
    if (off + 8 > len) return 0;
    *np = ld32(b + off);
    *nc = ld32(b + off + 4);
    if (*np > 4096 || *nc > 4096) return 0;
    return off + 8;
}

static size_t dtn_props_end(const uint8_t *b, size_t len, size_t off) {
    uint32_t np, nc;
    size_t p = dtn_hdr(b, len, off, &np, &nc);
    if (!p) return 0;
    for (uint32_t i = 0; i < np; i++) {
        if (p + 36 > len) return 0;
        uint32_t l = ld32(b + p + 32) & 0x7fffffffu;
        p += 36 + ((l + 3u) & ~3u);
        if (p > len) return 0;
    }
    return p;
}

static size_t dtn_end(const uint8_t *b, size_t len, size_t off, unsigned depth) {
    uint32_t np, nc;
    if (depth > 32) return 0;
    if (!dtn_hdr(b, len, off, &np, &nc)) return 0;
    size_t p = dtn_props_end(b, len, off);
    for (uint32_t i = 0; p && i < nc; i++) p = dtn_end(b, len, p, depth + 1);
    return p;
}

static uint8_t *dtn_prop(uint8_t *b, size_t len, size_t off,
                         const char *name, uint32_t *vlen) {
    uint32_t np, nc;
    size_t p = dtn_hdr(b, len, off, &np, &nc);
    if (!p) return NULL;
    for (uint32_t i = 0; i < np; i++) {
        if (p + 36 > len) return NULL;
        char nm[DTNAME + 1];
        memcpy(nm, b + p, DTNAME); nm[DTNAME] = '\0';
        uint32_t l = ld32(b + p + 32) & 0x7fffffffu;
        if (p + 36 + (size_t)l > len) return NULL;
        if (!strcmp(nm, name)) { if (vlen) *vlen = l; return b + p + 36; }
        p += 36 + ((l + 3u) & ~3u);
    }
    return NULL;
}

#define DT_NONE ((size_t)-1)

static size_t dtn_path(uint8_t *b, size_t len, const char *path) {
    size_t off = 0;
    while (path && *path) {
        while (*path == '/') path++;
        if (!*path) break;
        const char *slash = strchr(path, '/');
        size_t clen = slash ? (size_t)(slash - path) : strlen(path);
        uint32_t np, nc;
        if (!dtn_hdr(b, len, off, &np, &nc)) return DT_NONE;
        size_t c = dtn_props_end(b, len, off);
        size_t found = DT_NONE;
        for (uint32_t i = 0; c && i < nc; i++) {
            uint32_t vl = 0;
            const uint8_t *nm = dtn_prop(b, len, c, "name", &vl);
            if (nm && vl >= clen && !memcmp(nm, path, clen) &&
                (vl == clen || nm[clen] == '\0')) { found = c; break; }
            c = dtn_end(b, len, c, 0);
        }
        if (found == DT_NONE) return DT_NONE;
        off = found;
        path = slash ? slash + 1 : path + clen;
    }
    return off;
}

static bool dt_set_u32(uint8_t *b, size_t len, const char *path,
                       const char *prop, uint32_t v) {
    size_t node = dtn_path(b, len, path);
    if (node == DT_NONE) return false;
    uint32_t vl = 0;
    uint8_t *p = dtn_prop(b, len, node, prop, &vl);
    if (!p || vl != 4) return false;
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    return true;
}

static bool dt_set_reg(uint8_t *b, size_t len, const char *path,
                       const char *prop, uint32_t base, uint32_t size) {
    size_t node = dtn_path(b, len, path);
    if (node == DT_NONE) return false;
    uint32_t vl = 0;
    uint8_t *p = dtn_prop(b, len, node, prop, &vl);
    if (!p || vl != 8) return false;
    uint32_t vals[2] = { base, size };
    for (int i = 0; i < 2; i++) {
        p[i*4+0] = (uint8_t)vals[i];         p[i*4+1] = (uint8_t)(vals[i] >> 8);
        p[i*4+2] = (uint8_t)(vals[i] >> 16); p[i*4+3] = (uint8_t)(vals[i] >> 24);
    }
    return true;
}

static bool dt_unmatch(uint8_t *b, size_t len, const char *path) {
    size_t node = dtn_path(b, len, path);
    if (node == DT_NONE) return false;
    uint32_t vl = 0;
    uint8_t *p = dtn_prop(b, len, node, "compatible", &vl);
    if (!p || !vl) return false;
    p[0] = 'x';
    return true;
}

static bool dt_memmap_add(uint8_t *b, size_t len, const char *key,
                          uint32_t addr, uint32_t size) {
    size_t node = dtn_path(b, len, "chosen/memory-map");
    if (node == DT_NONE) return false;
    uint32_t np, nc;
    size_t p = dtn_hdr(b, len, node, &np, &nc);
    if (!p) return false;
    size_t slot = 0;
    for (uint32_t i = 0; i < np; i++) {
        if (p + 36 > len) return false;
        char nm[DTNAME + 1];
        memcpy(nm, b + p, DTNAME); nm[DTNAME] = '\0';
        uint32_t l = ld32(b + p + 32) & 0x7fffffffu;
        if (!strcmp(nm, key) && l == 8) { slot = p; break; }
        if (!slot && l == 8 && !strncmp(nm, "MemoryMapReserved-", 18) &&
            ld32(b + p + 36) == 0 && ld32(b + p + 40) == 0)
            slot = p;
        p += 36 + ((l + 3u) & ~3u);
        if (p > len) return false;
    }
    if (!slot) return false;
    memset(b + slot, 0, DTNAME);
    memcpy(b + slot, key, strlen(key) < DTNAME ? strlen(key) : DTNAME - 1);
    uint8_t *v = b + slot + 36;
    uint32_t vals[2] = { addr, size };
    for (int i = 0; i < 2; i++) {
        v[i*4+0] = (uint8_t)vals[i];         v[i*4+1] = (uint8_t)(vals[i] >> 8);
        v[i*4+2] = (uint8_t)(vals[i] >> 16); v[i*4+3] = (uint8_t)(vals[i] >> 24);
    }
    return true;
}

/* Clock rates, verbatim from bootkernel — see the long note there for sources. */
#define SB_CPU_HZ  412000000u
#define SB_BUS_HZ  103000000u
#define SB_MEM_HZ  103000000u
#define SB_PRF_HZ   51500000u
#define SB_FIX_HZ   24000000u
#define SB_TB_HZ     6000000u

static const struct { const char *path, *prop; uint32_t val; } DT_PATCH[] = {
    { "",          "clock-frequency",      SB_BUS_HZ },
    { "cpus/cpu0", "timebase-frequency",   SB_TB_HZ  },
    { "cpus/cpu0", "clock-frequency",      SB_CPU_HZ },
    { "cpus/cpu0", "bus-frequency",        SB_BUS_HZ },
    { "cpus/cpu0", "memory-frequency",     SB_MEM_HZ },
    { "cpus/cpu0", "peripheral-frequency", SB_PRF_HZ },
    { "cpus/cpu0", "fixed-frequency",      SB_FIX_HZ },
};
#define NDTPATCH ((unsigned)(sizeof DT_PATCH / sizeof DT_PATCH[0]))

/* --------------------------------------------------------------------------
 * The machine report. Everything printed here is a function of s5l8900_t and
 * nothing else, so two runs that reach the same instruction count must print
 * the same bytes — whether they got there in one process or two.
 * ------------------------------------------------------------------------ */
static void report(const s5l8900_t *m, arm_status_t st) {
    printf("=== MACHINE AT %llu RETIRED INSTRUCTIONS ===\n",
           (unsigned long long)m->cpu.cycles);
    printf("  status        : %d\n", (int)st);
    printf("  pc            : 0x%08x\n", m->cpu.r[15]);
    printf("  cpsr          : 0x%08x\n", m->cpu.cpsr);
    for (int i = 0; i < 16; i += 4)
        printf("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n",
               i, m->cpu.r[i], i+1, m->cpu.r[i+1], i+2, m->cpu.r[i+2],
               i+3, m->cpu.r[i+3]);
    for (unsigned i = 0; i < ARM_BANK_COUNT; i++)
        printf("  bank%u  r13 %08x  r14 %08x  spsr %08x\n",
               i, m->cpu.bank_r13[i], m->cpu.bank_r14[i], m->cpu.spsr[i]);
    printf("  fiq r8-12     : %08x %08x %08x %08x %08x\n",
           m->cpu.fiq_r8_12[0], m->cpu.fiq_r8_12[1], m->cpu.fiq_r8_12[2],
           m->cpu.fiq_r8_12[3], m->cpu.fiq_r8_12[4]);
    printf("  usr r8-12     : %08x %08x %08x %08x %08x\n",
           m->cpu.usr_r8_12[0], m->cpu.usr_r8_12[1], m->cpu.usr_r8_12[2],
           m->cpu.usr_r8_12[3], m->cpu.usr_r8_12[4]);
    printf("  sctlr/ttbr0   : 0x%08x / 0x%08x\n", m->cpu.cp15.sctlr, m->cpu.cp15.ttbr0);
    printf("  ttbr1/ttbcr   : 0x%08x / 0x%08x\n", m->cpu.cp15.ttbr1, m->cpu.cp15.ttbcr);
    printf("  dacr/ctxid    : 0x%08x / 0x%08x\n", m->cpu.cp15.dacr, m->cpu.cp15.context_id);
    printf("  DFSR/DFAR     : 0x%08x / 0x%08x\n", m->cpu.cp15.dfsr, m->cpu.cp15.dfar);
    printf("  IFSR/IFAR     : 0x%08x / 0x%08x\n", m->cpu.cp15.ifsr, m->cpu.cp15.ifar);
    printf("  tpidr rw/ro/p : %08x %08x %08x\n", m->cpu.cp15.tpidrurw,
           m->cpu.cp15.tpidruro, m->cpu.cp15.tpidrprw);
    printf("  excl          : valid=%d addr=0x%08x\n", m->cpu.excl_valid, m->cpu.excl_addr);
    printf("  vfp exc/scr   : %08x %08x\n", m->cpu.vfp_fpexc, m->cpu.vfp_fpscr);
    printf("  lines         : irq=%d fiq=%d  abort_pending=%d fsr=%08x far=%08x\n",
           m->cpu.irq_line, m->cpu.fiq_line, m->cpu.abort_pending,
           m->cpu.abort_fsr, m->cpu.abort_far);
    for (unsigned v = 0; v < S5L8900_VIC_COUNT; v++)
        printf("  VIC%u          : raw=%08x soft=%08x en=%08x sel=%08x\n",
               v, m->vic[v].raw, m->vic[v].soft, m->vic[v].enable, m->vic[v].select);
    printf("  timer         : ticks=%llu latch=%08x cfg=%08x t4=%08x/%08x "
           "cnt=%u/%u val=%u\n",
           (unsigned long long)m->timer.ticks, m->timer.irqlatch, m->timer.config,
           m->timer.t4_config, m->timer.t4_state, m->timer.t4_count,
           m->timer.t4_count2, m->timer.t4_value);
    printf("  tb_accum      : %llu\n", (unsigned long long)m->tb_accum);
    printf("  power         : state=%08x cfg0=%08x cfg1=%08x sram=%08x\n",
           m->power.state, m->power.cfg0, m->power.cfg1, m->power.sram);
    printf("  clcd          : ctrl=%08x status=%08x mask=%08x scanning=%d frames=%llu\n",
           m->clcd.ctrl, m->clcd.intstatus, m->clcd.intmask, m->clcd.scanning,
           (unsigned long long)m->clcd.frames);
    printf("  tvout        : running=%d irq30=%d frame=%u/%u frames=%llu "
           "ctrl=%08x mixer=%08x sdo=%08x pending/mask=%08x/%08x\n",
           s5l_tvout_running(&m->tvout), s5l_tvout_irq(&m->tvout),
           m->tvout.frame_accum, m->tvout.frame_ticks,
           (unsigned long long)m->tvout.frames,
           s5l_tvout_read(&m->tvout, S5L_TVOUT_BANK_CTRL, 0u, 4u),
           s5l_tvout_read(&m->tvout, S5L_TVOUT_BANK_MIXER, 0u, 4u),
           s5l_tvout_read(&m->tvout, S5L_TVOUT_BANK_SDO, 0u, 4u),
           s5l_tvout_read(&m->tvout, S5L_TVOUT_BANK_SDO,
                          TVOUT_SDO_IRQ, 4u),
           s5l_tvout_read(&m->tvout, S5L_TVOUT_BANK_SDO,
                          TVOUT_SDO_IRQMASK, 4u));
    printf("  unmapped      : reads=%llu writes=%llu pages=%u\n",
           (unsigned long long)m->unmapped_reads,
           (unsigned long long)m->unmapped_writes, m->unmapped_addr_count);
    for (unsigned i = 0; i < m->unmapped_addr_count; i++)
        printf("    0x%08x\n", m->unmapped_addr[i]);
    for (unsigned i = 0; i < m->stub_count; i++)
        printf("  stub %-10s r=%llu w=%llu oob=%llu\n", m->stubs[i].name,
               (unsigned long long)m->stubs[i].reads,
               (unsigned long long)m->stubs[i].writes,
               (unsigned long long)m->stubs[i].oob);
    printf("=== KERNEL UART OUTPUT (%zu bytes) ===\n", m->uart0.tx_len);
    fwrite(m->uart0.tx, 1, m->uart0.tx_len, stdout);
    printf("\n=== END ===\n");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s <kernel.macho> [-d dt.bin] [-c cmdline] [-r ramdisk]\n"
            "          [-R ram-MB] [-n steps] [-p physbase] [-V virtbase]\n"
            "          [--snapshot-at <steps> <file>]  [--restore <file>]\n", argv[0]);
        return 1;
    }
    uint32_t phys_base = S5L8900_SDRAM_BASE;
    uint32_t virt_base = 0xc0000000u;
    uint64_t steps = 2000000ull;
    const char *dtpath = NULL, *rdpath = NULL, *restore = NULL;
    const char *cmdline = "debug=0x8 serial=1";
    uint32_t ram_size = 128u << 20;
    unsigned ba_rev = 1, ba_ver = 6;

    struct { uint64_t at; const char *path; } snaps[8];
    unsigned nsnaps = 0;

    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--snapshot-at") && i + 2 < argc) {
            if (nsnaps < 8) {
                snaps[nsnaps].at   = strtoull(argv[i + 1], NULL, 0);
                snaps[nsnaps].path = argv[i + 2];
                nsnaps++;
            }
            i += 2; continue;
        }
        if (i + 1 >= argc) break;
        if      (!strcmp(argv[i], "--restore")) restore  = argv[++i];
        else if (!strcmp(argv[i], "-p")) phys_base = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-V")) virt_base = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-n")) steps     = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-d")) dtpath    = argv[++i];
        else if (!strcmp(argv[i], "-c")) cmdline   = argv[++i];
        else if (!strcmp(argv[i], "-r")) rdpath    = argv[++i];
        else if (!strcmp(argv[i], "-R")) ram_size  = (uint32_t)strtoul(argv[++i], NULL, 0) << 20;
    }

    /* The kernel's static map tops out at 0xe0000000; see bootkernel. */
    const uint32_t KERN_MEMSIZE_MAX = 0x20000000u;
    if (ram_size > KERN_MEMSIZE_MAX) ram_size = KERN_MEMSIZE_MAX;

    size_t len = 0;
    uint8_t *img = slurp(argv[1], &len);
    if (!img) return 1;

    macho_t mo;
    macho_status_t mst = macho_parse(img, len, &mo);
    if (mst != MACHO_OK) { fprintf(stderr, "macho: %s\n", macho_strerror(mst)); return 2; }
    if (!mo.has_entry)   { fprintf(stderr, "no entry point\n"); return 2; }

    s5l8900_t mach;
    if (!s5l8900_init(&mach, phys_base, ram_size)) { fprintf(stderr, "init failed\n"); return 1; }
    mach.trace_devices = true;

    for (unsigned i = 0; i < mo.segment_count; i++) {
        macho_segment_t *s = &mo.segments[i];
        if (!s->filesize || s->vmaddr < virt_base) continue;
        s5l8900_load(&mach, s->vmaddr - virt_base + phys_base, img + s->fileoff, s->filesize);
    }

    /* The IORTC wait patch — without it the boot idles for 30 guest seconds. */
    {
        uint32_t pa = 0xc0175b3eu - virt_base + phys_base;
        uint8_t got = mach.bus.read8(mach.bus.ctx, pa);
        if (got == 0x1e) mach.bus.write8(mach.bus.ctx, pa, 0x00);
        else fprintf(stderr, "note: kpatch site holds %02x, not 1e — skipped\n", got);
    }

    uint32_t dt_pa = (mo.vm_high - virt_base + phys_base + 0xfffu) & ~0xfffu;
    size_t dt_n = 0;
    uint8_t *dt = dtpath ? slurp(dtpath, &dt_n) : NULL;
    uint32_t dt_len = dt ? (uint32_t)dt_n : 0;

    size_t rd_n = 0;
    uint8_t *rd = rdpath ? slurp(rdpath, &rd_n) : NULL;
    if (rdpath && !rd) { fprintf(stderr, "ramdisk: cannot read %s\n", rdpath); return 1; }

    uint32_t ba_pa  = (dt_pa + dt_len + 0xfffu) & ~0xfffu;
    uint32_t rd_pa  = (ba_pa + 0x1000u + 0xfffu) & ~0xfffu;
    uint32_t rd_len = (uint32_t)((rd_n + 0xfffu) & ~(size_t)0xfffu);
    uint32_t top_of_kernel_data = rd ? ((rd_pa + rd_len + 0x3fffu) & ~0x3fffu)
                                     : ((ba_pa + 0x1000u + 0x3fffu) & ~0x3fffu);
    uint32_t rd_dt_addr = rd_pa - phys_base + virt_base;

    if (dt) {
        for (unsigned i = 0; i < NDTPATCH; i++)
            dt_set_u32(dt, dt_n, DT_PATCH[i].path, DT_PATCH[i].prop, DT_PATCH[i].val);
        dt_set_reg(dt, dt_n, "memory", "reg", phys_base, ram_size);
        if (rd) dt_memmap_add(dt, dt_n, "RAMDisk", rd_dt_addr, rd_len);
        dt_unmatch(dt, dt_n, "arm-io/mbx");
        s5l8900_load(&mach, dt_pa, dt, dt_n);
        free(dt);
    }
    if (rd) { s5l8900_load(&mach, rd_pa, rd, rd_n); free(rd); }

    char cmdbuf[512];
    if (rd && !strstr(cmdline, "rd=")) {
        snprintf(cmdbuf, sizeof cmdbuf, "%s%srd=md0", cmdline, *cmdline ? " " : "");
        cmdline = cmdbuf;
    }

    uint8_t ba[0x138];
    memset(ba, 0, sizeof ba);
#define PUT16(o, v) do { ba[o] = (uint8_t)(v); ba[(o)+1] = (uint8_t)((v) >> 8); } while (0)
#define PUT32(o, v) do { ba[o] = (uint8_t)(v); ba[(o)+1] = (uint8_t)((v) >> 8); \
                         ba[(o)+2] = (uint8_t)((v) >> 16); ba[(o)+3] = (uint8_t)((v) >> 24); } while (0)
    PUT16(0x00, ba_rev);
    PUT16(0x02, ba_ver);
    PUT32(0x04, virt_base);
    PUT32(0x08, phys_base);
    PUT32(0x0c, ram_size);
    PUT32(0x10, top_of_kernel_data);
    PUT32(0x14, 0);                      /* v_baseAddr: no framebuffer */
    PUT32(0x18, 0);                      /* v_display: kernel text console */
    PUT32(0x1c, 320 * 4);
    PUT32(0x20, 320);
    PUT32(0x24, 480);
    PUT32(0x28, 32);
    PUT32(0x2c, 0);
    PUT32(0x30, dt_len ? (dt_pa - phys_base + virt_base) : 0);
    PUT32(0x34, dt_len);
    memcpy(ba + 0x38, cmdline, strlen(cmdline) < 255 ? strlen(cmdline) : 255);
#undef PUT16
#undef PUT32
    s5l8900_load(&mach, ba_pa, ba, sizeof ba);

    mach.cpu.r[15] = mo.entry - virt_base + phys_base;
    mach.cpu.r[0]  = ba_pa;

    if (restore) {
        snapshot_status_t rs = snapshot_load(&mach, restore);
        if (rs != SNAP_OK) {
            fprintf(stderr, "restore %s: %s\n", restore, snapshot_strerror(rs));
            return 3;
        }
        fprintf(stderr, "restored %s at %llu instructions\n",
                restore, (unsigned long long)mach.cpu.cycles);
    }

    arm_status_t st = ARM_OK;
    uint64_t n = mach.cpu.cycles;      /* continue from where the machine is */
    for (; n < steps; n++) {
        st = arm_step(&mach.cpu);
        if (st != ARM_OK) { n++; break; }
        s5l8900_tick(&mach, 1);
        for (unsigned s = 0; s < nsnaps; s++) {
            if (snaps[s].at != mach.cpu.cycles) continue;
            snapshot_status_t ss = snapshot_save(&mach, snaps[s].path);
            fprintf(stderr, "snapshot @%llu -> %s: %s\n",
                    (unsigned long long)mach.cpu.cycles, snaps[s].path,
                    snapshot_strerror(ss));
            if (ss != SNAP_OK) return 4;
        }
    }

    report(&mach, st);
    s5l8900_free(&mach);
    free(img);
    return 0;
}

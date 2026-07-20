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

/* ---------------------------------------------------------------------------
 * Symbol table (LC_SYMTAB) reader.
 *
 * The kernelcache still carries its full nlist symbol table, so every address
 * we print can be named. Without this a panic report is a list of hex numbers.
 * ------------------------------------------------------------------------- */
#define DTNAME 32                 /* device-tree property names are char[32] */

typedef struct { uint32_t value; const char *name; } ksym_t;
static ksym_t   *ksyms;
static unsigned  ksym_count;

static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ksym_cmp(const void *a, const void *b) {
    uint32_t x = ((const ksym_t *)a)->value, y = ((const ksym_t *)b)->value;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void ksyms_load(const uint8_t *img, size_t len) {
    if (len < 28) return;
    uint32_t ncmds = ld32(img + 16);
    size_t off = 28;
    for (uint32_t i = 0; i < ncmds && off + 8 <= len; i++) {
        uint32_t cmd = ld32(img + off), csz = ld32(img + off + 4);
        if (!csz) break;
        if (cmd == 2 /* LC_SYMTAB */ && off + 24 <= len) {
            uint32_t symoff = ld32(img + off + 8), nsyms = ld32(img + off + 12);
            uint32_t stroff = ld32(img + off + 16), strsize = ld32(img + off + 20);
            if ((size_t)symoff + (size_t)nsyms * 12 > len) return;
            if ((size_t)stroff + strsize > len) return;
            ksyms = calloc(nsyms ? nsyms : 1, sizeof *ksyms);
            if (!ksyms) return;
            for (uint32_t k = 0; k < nsyms; k++) {
                const uint8_t *e = img + symoff + k * 12;
                uint32_t strx = ld32(e);
                uint8_t  type = e[4];
                uint32_t val  = ld32(e + 8);
                if ((type & 0x0e) != 0x0e || !val) continue;   /* N_SECT only */
                if (strx >= strsize) continue;
                ksyms[ksym_count].value = val;
                ksyms[ksym_count].name  = (const char *)(img + stroff + strx);
                ksym_count++;
            }
            qsort(ksyms, ksym_count, sizeof *ksyms, ksym_cmp);
            return;
        }
        off += csz;
    }
}

/* Name an address as "sym+0x.."; returns a pointer to a rotating static buffer
 * so several calls can appear in one printf. */
static const char *ksym_at(uint32_t addr) {
    static char buf[4][160];
    static unsigned turn;
    char *out = buf[turn++ & 3];
    addr &= ~1u;                       /* drop the Thumb bit */
    if (!ksym_count) { snprintf(out, 160, "?"); return out; }
    unsigned lo = 0, hi = ksym_count;  /* last symbol with value <= addr */
    while (lo < hi) { unsigned mid = (lo + hi) / 2;
        if (ksyms[mid].value <= addr) lo = mid + 1; else hi = mid; }
    if (!lo) { snprintf(out, 160, "?"); return out; }
    const ksym_t *s = &ksyms[lo - 1];
    uint32_t d = addr - s->value;
    if (d > 0x8000u) { snprintf(out, 160, "?"); return out; }
    if (d) snprintf(out, 160, "%s+0x%x", s->name, d);
    else   snprintf(out, 160, "%s", s->name);
    return out;
}

static uint32_t ksym_value(const char *name) {
    for (unsigned i = 0; i < ksym_count; i++)
        if (!strcmp(ksyms[i].name, name)) return ksyms[i].value;
    return 0;
}

/* ===========================================================================
 * Device-tree patching — standing in for iBoot.
 *
 * The device tree shipped in the IPSW is a TEMPLATE. On real hardware iBoot
 * measures the PLLs and writes the actual clock rates into it before handing
 * it to the kernel; every frequency property in the file on disk is zero.
 * pe_identify_machine() copies those zeros into gPEClockFrequencyInfo, and the
 * kernel then divides by them:
 *
 *   pe_identify_machine+0xbe:  bus_to_cpu_rate_num = (cpu_clock_hz * 2) / bus_clock_hz
 *   pe_identify_machine+0xd0:  bus_to_dec_rate_den = bus_clock_hz / dec_clock_hz
 *   rtclock.c:132              panic if timebase_num < timebase_den
 *
 * so we do iBoot's job here. Patching is IN PLACE and SAME-LENGTH: the flat
 * format has no relocation table but every offset is implicit in the byte
 * stream, so resizing a property would mean rebuilding the whole blob.
 *
 * Format (Apple's, not FDT):
 *   node     := u32 nProperties, u32 nChildren, property[], node[]
 *   property := char name[32], u32 length (bit31 is a tool flag), value[],
 *               padded up to a 4-byte boundary
 * ------------------------------------------------------------------------- */

static size_t dtn_hdr(const uint8_t *b, size_t len, size_t off,
                      uint32_t *np, uint32_t *nc) {
    if (off + 8 > len) return 0;
    *np = ld32(b + off);
    *nc = ld32(b + off + 4);
    if (*np > 4096 || *nc > 4096) return 0;   /* corrupt-input guard */
    return off + 8;
}

/* Offset just past the last property of the node at `off`. */
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

/* Offset just past the whole subtree rooted at `off`. */
static size_t dtn_end(const uint8_t *b, size_t len, size_t off, unsigned depth) {
    uint32_t np, nc;
    if (depth > 32) return 0;
    if (!dtn_hdr(b, len, off, &np, &nc)) return 0;
    size_t p = dtn_props_end(b, len, off);
    for (uint32_t i = 0; p && i < nc; i++) p = dtn_end(b, len, p, depth + 1);
    return p;
}

/* Writable pointer to a property's value on the node at `off`. */
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

/* Walk a slash-separated path of node "name" properties from the root.
 * "" is the root itself; "cpus/cpu0" is the CPU node. */
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

/* Overwrite a 4-byte property in place. Refuses to change its length. */
static bool dt_set_u32(uint8_t *b, size_t len, const char *path,
                       const char *prop, uint32_t v) {
    size_t node = dtn_path(b, len, path);
    if (node == DT_NONE) {
        printf("  dt: node /%-22s NOT FOUND (skipping %s)\n", path, prop);
        return false;
    }
    uint32_t vl = 0;
    uint8_t *p = dtn_prop(b, len, node, prop, &vl);
    if (!p) {
        printf("  dt: /%s: property %s NOT FOUND\n", path, prop);
        return false;
    }
    if (vl != 4) {
        printf("  dt: /%s:%s is %u bytes, not 4 — refusing to resize\n",
               path, prop, vl);
        return false;
    }
    uint32_t old = ld32(p);
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
    printf("  dt: /%-14s %-22s 0x%08x -> 0x%08x (%u)\n",
           *path ? path : "device-tree", prop, old, v, v);
    return true;
}

/* Overwrite two consecutive 32-bit cells (an 8-byte "reg" entry) in place. */
static bool dt_set_reg(uint8_t *b, size_t len, const char *path,
                       const char *prop, uint32_t base, uint32_t size) {
    size_t node = dtn_path(b, len, path);
    if (node == DT_NONE) { printf("  dt: node /%s NOT FOUND\n", path); return false; }
    uint32_t vl = 0;
    uint8_t *p = dtn_prop(b, len, node, prop, &vl);
    if (!p || vl != 8) {
        printf("  dt: /%s:%s missing or not 8 bytes (%u)\n", path, prop, vl);
        return false;
    }
    uint32_t o0 = ld32(p), o1 = ld32(p + 4);
    uint32_t vals[2] = { base, size };
    for (int i = 0; i < 2; i++) {
        p[i*4+0] = (uint8_t)vals[i];        p[i*4+1] = (uint8_t)(vals[i] >> 8);
        p[i*4+2] = (uint8_t)(vals[i] >> 16); p[i*4+3] = (uint8_t)(vals[i] >> 24);
    }
    printf("  dt: /%-14s %-22s {0x%08x,0x%08x} -> {0x%08x,0x%08x}\n",
           path, prop, o0, o1, base, size);
    return true;
}

/*
 * S5L8900 clock rates.
 *
 * RESEARCHED (two independent sources agree):
 *   TB_HZ  6 MHz  — openiBoot plat-s5l8900/clock.c computes
 *                   TimebaseFrequency = FREQUENCY_BASE / 2 with
 *                   FREQUENCY_BASE == 12000000, and measurements of
 *                   mach_absolute_time() on the original iPhone and the
 *                   iPhone 3G report a 6 MHz tick (the 3GS and later are
 *                   24 MHz / the familiar 125:3 timebase). This is the value
 *                   the rtclock panic is actually about.
 *   FIX_HZ 24 MHz — same file: FixedFrequency = FREQUENCY_BASE * 2.
 *   CPU_HZ 412 MHz— the universally documented iPhone 3G core clock
 *                   (ARM1176JZF-S underclocked from ~620 MHz).
 *
 * PROVISIONAL / DERIVED — I could not find a dump of a real iBoot-populated
 * iPhone1,2 tree, and openiBoot reads these out of the PLL registers at run
 * time rather than hard-coding them. These are the self-consistent ratios that
 * fall out of the S5L8900 clock tree (AHB = core/4, peripheral = AHB/2), and
 * nothing the kernel does with them is sensitive to the exact value — only to
 * their being non-zero and to (2*CPU)/BUS being a sane small integer:
 *   BUS_HZ 103 MHz = CPU/4        (bus_to_cpu_rate comes out 8/2 == 4)
 *   MEM_HZ 103 MHz                (LPDDR clock; assumed equal to the bus)
 *   PRF_HZ 51.5 MHz = BUS/2
 * If a real iPhone1,2 IORegistry dump ever turns up, replace these three.
 */
#define S5L8900_CPU_HZ  412000000u
#define S5L8900_BUS_HZ  103000000u
#define S5L8900_MEM_HZ  103000000u
#define S5L8900_PRF_HZ   51500000u
#define S5L8900_FIX_HZ   24000000u
#define S5L8900_TB_HZ     6000000u

/* Applied in order; later entries win, and -D on the command line wins over
 * all of them. Nothing here changes firmware/devicetree.bin on disk — this
 * runs on the copy that is about to be loaded into guest RAM. */
static const struct { const char *path, *prop; uint32_t val; } DT_PATCH[] = {
    /* Root clock-frequency. PROVISIONAL: I have not found anything in
     * xnu-1357 that reads it, and no dump that says what iBoot puts there;
     * the SoC/bus clock is the plausible reading of the name. */
    { "",          "clock-frequency",      S5L8900_BUS_HZ },
    { "cpus/cpu0", "timebase-frequency",   S5L8900_TB_HZ  },
    { "cpus/cpu0", "clock-frequency",      S5L8900_CPU_HZ },
    { "cpus/cpu0", "bus-frequency",        S5L8900_BUS_HZ },
    { "cpus/cpu0", "memory-frequency",     S5L8900_MEM_HZ },
    { "cpus/cpu0", "peripheral-frequency", S5L8900_PRF_HZ },
    { "cpus/cpu0", "fixed-frequency",      S5L8900_FIX_HZ },
};
#define NDTPATCH ((unsigned)(sizeof DT_PATCH / sizeof DT_PATCH[0]))

/* ---------------------------------------------------------------------------
 * Reading guest memory from the host, bypassing the MMU.
 *
 * Every address the kernel hands to panic() lives in the flat kernel window,
 * so the linear virt->phys relation is enough and cannot itself fault.
 * ------------------------------------------------------------------------- */
static s5l8900_t *g_mach;
static uint32_t   g_virt_base, g_phys_base;

static const uint8_t *guest_ptr(uint32_t va, uint32_t need) {
    uint32_t pa = va - g_virt_base + g_phys_base;
    if (va < g_virt_base) pa = va;                     /* already physical */
    if (pa < g_mach->ram_base) return NULL;
    uint32_t o = pa - g_mach->ram_base;
    if (o >= g_mach->ram_size || g_mach->ram_size - o < need) return NULL;
    return g_mach->ram + o;
}

/* Print the C string at a guest address, escaped, or say why we cannot. */
static void print_guest_str(const char *label, uint32_t va) {
    const uint8_t *p = guest_ptr(va, 1);
    if (!p) { printf("  %s 0x%08x -> (not in RAM)\n", label, va); return; }
    printf("  %s 0x%08x -> \"", label, va);
    for (unsigned i = 0; i < 400 && p[i]; i++) {
        uint8_t c = p[i];
        if (c == '\n') printf("\\n");
        else if (c == '\t') printf("\\t");
        else if (c >= 0x20 && c < 0x7f) putchar((char)c);
        else printf("\\x%02x", c);
    }
    printf("\"\n");
}

/* ------------------------------------------------------------------------
 * DIAGNOSTIC INSTRUMENTATION (temporary; for finding the serial console).
 *
 * Two questions we cannot answer from outside:
 *   1. Does the kernel ever touch the UART page at all?
 *   2. How far down XNU's console-init chain does it actually get?
 *
 * (1) is answered by wrapping the bus callbacks — bootkernel owns the
 *     arm_bus_t after s5l8900_init(), so it can interpose without touching
 *     the core. Note that with the MMU on, an access to an *unmapped virtual*
 *     address aborts inside the MMU and never reaches the bus, which is why
 *     "zero unmapped physical accesses" proves nothing on its own.
 *
 * (2) is answered by watching the PC for the addresses of the pexpert
 *     functions in the console path, taken from the kernel's own symbol
 *     table (xnu-1357.5.30, iPhone1,2 7E18). Each is matched both at its
 *     virtual address and at the physical alias it has before the MMU is on.
 * ---------------------------------------------------------------------- */
#define UARTPG 0x3cc00000u

typedef struct { const char *name; uint32_t va; } milestone_t;

/* Virtual addresses from the kernelcache symbol table (thumb bit cleared). */
static const milestone_t MILESTONES[] = {
    { "_arm_init_cpu",              0xc0062c58u },
    { "_arm_vm_init",               0xc0062d60u },
    { "_DTInit",                    0xc01a7474u },
    { "_PE_init_platform",          0xc01a83ecu },
    { "_pe_identify_machine",       0xc01a7f1cu },
    { "_pe_arm_get_soc_base_phys",  0xc01a7c5cu },
    { "_PE_parse_boot_argn",        0xc01a7a5au },
    { "_PE_init_kprintf",           0xc01a8840u },
    { "_serial_init",               0xc01a8a78u },
    { "serial_init:soc_base_call",  0xc01a8af4u },
    { "serial_init:RET0_no_soc",    0xc01a8afcu },
    { "serial_init:found_node",     0xc01a8b84u },
    { "serial_init:ml_io_map_call", 0xc01a8ba4u },
    { "serial_init:RET1_ok",        0xc01a8b78u },
    { "_ml_io_map",                 0xc006963cu },
    { "_DTFindEntry",               0xc01a7738u },
    { "_DTGetProperty",             0xc01a7620u },
    { "_uart_putc",                 0xc01a89d4u },
    { "uart_putc:past_init_gate",   0xc01a89f0u },
    { "_serial_putc",               0xc01a87f2u },
    { "_kprintf",                   0xc01a880cu },
    { "_cnputc",                    0xc006d342u },
    { "_conslog_putc",              0xc002290cu },
    { "_switch_to_serial_console",  0xc005f018u },
    { "_initialize_screen",         0xc0070380u },
    { "_PE_initialize_console",     0xc01a85f0u },
    { "_machine_startup",           0xc0209a40u },
    { "_kernel_bootstrap",          0xc020862cu },
    { "_bsd_init",                  0xc020ae14u },
    { "_panic",                     0xc001c13cu },
    { "_Debugger",                  0xc006abbcu },
};
#define NMILE ((unsigned)(sizeof MILESTONES / sizeof MILESTONES[0]))

static struct {
    /* bus interposition */
    arm_bus_t   inner;              /* the machine's original callbacks */
    s5l8900_t  *mach;
    uint64_t    uart_hits;
    unsigned    uart_log_n;
    struct { uint32_t addr, val, pc; bool wr; unsigned bytes; } uart_log[64];

    /* every distinct non-RAM page the guest reached, with the first PC */
    unsigned    dev_page_n;
    struct { uint32_t page, first_pc; uint64_t reads, writes; } dev_page[64];

    /* milestones */
    uint32_t    mile_pa[NMILE];
    uint64_t    mile_hits[NMILE];
    unsigned    mile_first[NMILE];

    /* distinct MMU faults */
    unsigned    fault_n;
    struct { uint32_t far_, fsr, pc; bool prefetch; unsigned first_at; uint64_t n; } fault[48];

    /* --- DIAGNOSTIC: exception returns that resume in Thumb state ---------
     * A "MOVS pc,lr" / "LDM ^" leaving an ARM handler for Thumb code must
     * resume at the exact halfword it was interrupted at. If the core forces
     * word alignment the resume address silently loses 2 bytes, and the guest
     * re-executes the previous halfword. Counting how many of these land on a
     * 4-aligned address is a direct test: genuine Thumb resume points should be
     * split roughly evenly between +0 and +2 mod 4. */
    uint64_t    exret_thumb, exret_thumb_aligned4, exret_mismatch;
    unsigned    exret_log_n;
    struct { unsigned at; uint32_t from_pc, lr, to_pc, mode_from, mode_to; } exret_log[24];

    /* --- DIAGNOSTIC: failed STREX/STREXD/STREXB/STREXH -------------------- */
    uint64_t    strex_total, strex_failed;
    unsigned    strex_log_n;
    struct { unsigned at; uint32_t pc, addr; } strex_log[32];
} G;

static void note_dev_page(uint32_t addr, uint32_t pc, bool wr) {
    uint32_t pg = addr & ~0xfffu;
    for (unsigned i = 0; i < G.dev_page_n; i++)
        if (G.dev_page[i].page == pg) {
            if (wr) G.dev_page[i].writes++; else G.dev_page[i].reads++;
            return;
        }
    if (G.dev_page_n < 64) {
        G.dev_page[G.dev_page_n].page = pg;
        G.dev_page[G.dev_page_n].first_pc = pc;
        G.dev_page[G.dev_page_n].reads  = wr ? 0 : 1;
        G.dev_page[G.dev_page_n].writes = wr ? 1 : 0;
        G.dev_page_n++;
    }
}

/* RAM test mirrors the core's: anything else is a device or a hole. */
static bool is_ram(uint32_t a, unsigned bytes) {
    if (a < G.mach->ram_base) return false;
    return (uint64_t)(a - G.mach->ram_base) + bytes <= (uint64_t)G.mach->ram_size;
}

static void spy(uint32_t addr, uint32_t val, unsigned bytes, bool wr) {
    if (is_ram(addr, bytes)) return;
    uint32_t pc = G.mach->cpu.r[15];
    note_dev_page(addr, pc, wr);
    if ((addr & ~0xfffu) == UARTPG) {
        G.uart_hits++;
        if (G.uart_log_n < 64) {
            G.uart_log[G.uart_log_n].addr  = addr;
            G.uart_log[G.uart_log_n].val   = val;
            G.uart_log[G.uart_log_n].pc    = pc;
            G.uart_log[G.uart_log_n].wr    = wr;
            G.uart_log[G.uart_log_n].bytes = bytes;
            G.uart_log_n++;
        }
    }
}

static uint32_t sr32(void *c, uint32_t a) { uint32_t v = G.inner.read32(c, a); spy(a, v, 4, false); return v; }
static uint16_t sr16(void *c, uint32_t a) { uint16_t v = G.inner.read16(c, a); spy(a, v, 2, false); return v; }
static uint8_t  sr8 (void *c, uint32_t a) { uint8_t  v = G.inner.read8 (c, a); spy(a, v, 1, false); return v; }
static void sw32(void *c, uint32_t a, uint32_t v) { spy(a, v, 4, true); G.inner.write32(c, a, v); }
static void sw16(void *c, uint32_t a, uint16_t v) { spy(a, v, 2, true); G.inner.write16(c, a, v); }
static void sw8 (void *c, uint32_t a, uint8_t  v) { spy(a, v, 1, true); G.inner.write8 (c, a, v); }

static void spy_install(s5l8900_t *m, uint32_t virt_base, uint32_t phys_base) {
    memset(&G, 0, sizeof G);
    G.mach  = m;
    G.inner = m->bus;
    m->bus.read32 = sr32; m->bus.read16 = sr16; m->bus.read8 = sr8;
    m->bus.write32 = sw32; m->bus.write16 = sw16; m->bus.write8 = sw8;
    for (unsigned i = 0; i < NMILE; i++)
        G.mile_pa[i] = MILESTONES[i].va - virt_base + phys_base;
}

static void note_fault(uint32_t far_, uint32_t fsr, uint32_t pc, bool pref, unsigned at) {
    for (unsigned i = 0; i < G.fault_n; i++)
        if (G.fault[i].far_ == far_ && G.fault[i].pc == pc && G.fault[i].prefetch == pref) {
            G.fault[i].n++; return;
        }
    if (G.fault_n < 48) {
        G.fault[G.fault_n].far_ = far_; G.fault[G.fault_n].fsr = fsr;
        G.fault[G.fault_n].pc = pc; G.fault[G.fault_n].prefetch = pref;
        G.fault[G.fault_n].first_at = at; G.fault[G.fault_n].n = 1;
        G.fault_n++;
    }
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
            "usage: %s <kernel.macho> [-p <physbase>] [-V <virtbase>] [-n <steps>]\n"
            "          [-d <devicetree.bin>] [-c <cmdline>] [-a] [-M]\n"
            "          [-D <node/path>:<prop>=<value>] ...\n"
            "  -D  patch a 4-byte device-tree property in the in-memory copy\n"
            "      (empty path == root), e.g. -D cpus/cpu0:timebase-frequency=6000000\n"
            "  -M  do not synthesise /memory reg from the RAM layout\n"
            "  -K  how many trace lines to print at the first data abort\n"
            "  -A  DIAGNOSTIC SHIM, not a fix: after an exception return that\n"
            "      resumes in Thumb state, undo the word alignment the core\n"
            "      applies to the resume address. Use it to confirm that a\n"
            "      failure is caused by that alignment, never to 'work'.\n",
            argv[0]);
        return 1;
    }
    uint32_t phys_base = S5L8900_SDRAM_BASE;
    uint32_t virt_base = 0xc0000000u;
    unsigned steps = 2000000u;
    const char *dtpath = NULL;
    const char *cmdline = "debug=0x8 serial=1";
    bool stop_on_abort = false;
    /* Advertising a framebuffer moves topOfKernelData and therefore where the
     * kernel places its page tables; that regressed the boot, so it is opt-in
     * until the interaction is understood. */
    bool want_fb = false;
    /* -A: from outside the core, undo the word-alignment the interpreter
     * applies when an exception return resumes in Thumb state. This is a
     * PROOF SHIM for a suspected core bug, not a fix. */
    /*
     * boot_args header. MEASURED, not guessed: pe_identify_machine() at
     * 0xc01a7f22 does `ldrh r3,[r0,#2]; cmp r3,#6` and panics
     * "pe_identify_machine: Epoch Mismatch" on anything else, so the halfword
     * at boot_args+2 must be 6. Revision (+0) is not checked here; 1 works.
     * Override with -r/-w to re-probe (-w 2 reproduces the original panic).
     */
    unsigned ba_rev = 1, ba_ver = 6;

    /* -D <path>:<prop>=<value> overrides / adds a device-tree patch, so the
     * frequencies can be probed from the shell instead of by rebuilding. */
    struct { const char *path, *prop; uint32_t val; } dtov[32];
    unsigned ndtov = 0;
    char dtbuf[32][96];
    bool patch_memnode = true;
    unsigned ktail = 512;              /* -K n: trace lines to print on abort */

    /* Walk the arguments one at a time: pair-stepping breaks as soon as a
     * single-argument flag like -a appears. */
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) { stop_on_abort = true; continue; }
        if (!strcmp(argv[i], "-F")) { want_fb = true; continue; }
        if (!strcmp(argv[i], "-M")) { patch_memnode = false; continue; }
        if (i + 1 >= argc) break;
        if (!strcmp(argv[i], "-D")) {
            if (ndtov >= 32) { i++; continue; }
            snprintf(dtbuf[ndtov], sizeof dtbuf[0], "%s", argv[++i]);
            char *colon = strchr(dtbuf[ndtov], ':');
            char *eq    = strchr(dtbuf[ndtov], '=');
            if (!colon || !eq || eq < colon) {
                fprintf(stderr, "-D wants <node/path>:<prop>=<value>\n");
                return 1;
            }
            *colon = '\0'; *eq = '\0';
            dtov[ndtov].path = dtbuf[ndtov];
            dtov[ndtov].prop = colon + 1;
            dtov[ndtov].val  = (uint32_t)strtoul(eq + 1, NULL, 0);
            ndtov++;
            continue;
        }
        if      (!strcmp(argv[i], "-p")) phys_base = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-V")) virt_base = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-n")) steps     = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-K")) ktail     = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-d")) dtpath    = argv[++i];
        else if (!strcmp(argv[i], "-c")) cmdline   = argv[++i];
        else if (!strcmp(argv[i], "-r")) ba_rev    = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-w")) ba_ver    = (unsigned)strtoul(argv[++i], NULL, 0);
    }

    size_t len = 0;
    uint8_t *img = slurp(argv[1], &len);
    if (!img) return 1;

    macho_t m;
    macho_status_t mst = macho_parse(img, len, &m);
    if (mst != MACHO_OK) { fprintf(stderr, "macho: %s\n", macho_strerror(mst)); return 2; }
    if (!m.has_entry)    { fprintf(stderr, "no entry point in the image\n"); return 2; }

    ksyms_load(img, len);
    printf("symbols    : %u\n", ksym_count);

    /* Enough RAM to hold the kernel plus room for its page tables and heap. */
    const uint32_t ram_size = 128u << 20;
    s5l8900_t mach;
    if (!s5l8900_init(&mach, phys_base, ram_size)) { fprintf(stderr, "init failed\n"); return 1; }
    mach.trace_devices = true;
    g_mach = &mach; g_virt_base = virt_base; g_phys_base = phys_base;

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
            printf("devicetree : %s -> pa 0x%08x (%u bytes)\n", dtpath, dt_pa, dt_len);
            /*
             * Stand in for iBoot: the shipped tree is a template whose clock
             * properties are all zero. Patch the in-memory copy only.
             */
            printf("  --- device-tree patches (iBoot would have done these) ---\n");
            for (unsigned i = 0; i < NDTPATCH; i++)
                dt_set_u32(dt, n, DT_PATCH[i].path, DT_PATCH[i].prop, DT_PATCH[i].val);
            /* /memory reg: the real iBoot fills in the DRAM bank. Ours is
             * zero, which would advertise a zero-sized bank. */
            if (patch_memnode)
                dt_set_reg(dt, n, "memory", "reg", phys_base, ram_size);
            for (unsigned i = 0; i < ndtov; i++)
                dt_set_u32(dt, n, dtov[i].path, dtov[i].prop, dtov[i].val);
            printf("  ---------------------------------------------------------\n");
            s5l8900_load(&mach, dt_pa, dt, n);
            free(dt);
        }
    }

    /*
     * Reserve a linear framebuffer and advertise it in Boot_Video. With
     * v_baseAddr left at 0 the kernel's PE_create_console reads address 0,
     * finds no framebuffer, and the video console silently goes nowhere — which
     * is why no boot text ever appeared. A real buffer lets the kernel render
     * its console into memory we can look at.
     */
    const uint32_t FB_W = 320, FB_H = 480, FB_BPP = 4;
    const uint32_t fb_bytes = want_fb ? FB_W * FB_H * FB_BPP : 0u;

    uint32_t ba_pa = (dt_pa + dt_len + 0xfffu) & ~0xfffu;

    /*
     * topOfKernelData is where the kernel starts placing its own page tables,
     * so anything counted below it moves them. Placing the framebuffer here
     * and then advancing topOfKernelData past it is what made -F regress into
     * a prefetch abort: the buffer itself was fine, but every table the kernel
     * built afterwards landed somewhere else.
     *
     * Real iBoot does not put the framebuffer immediately after the kernel; it
     * sits near the top of DRAM, far above the kernel's data. Do the same, and
     * leave topOfKernelData describing only the kernel's own data.
     */
    uint32_t top_of_kernel_data = (ba_pa + 0x1000u + 0xfffu) & ~0xfffu;
    uint32_t fb_pa = want_fb
        ? ((phys_base + ram_size - fb_bytes) & ~0xfffu)   /* top of DRAM */
        : top_of_kernel_data;

    uint8_t ba[0x138];
    memset(ba, 0, sizeof ba);
#define PUT16(o, v) do { ba[o] = (uint8_t)(v); ba[(o)+1] = (uint8_t)((v) >> 8); } while (0)
#define PUT32(o, v) do { ba[o] = (uint8_t)(v); ba[(o)+1] = (uint8_t)((v) >> 8); \
                         ba[(o)+2] = (uint8_t)((v) >> 16); ba[(o)+3] = (uint8_t)((v) >> 24); } while (0)
    PUT16(0x00, ba_rev);               /* Revision  */
    PUT16(0x02, ba_ver);               /* Version   */
    PUT32(0x04, virt_base);
    PUT32(0x08, phys_base);
    PUT32(0x0c, ram_size);
    /* PHYSICAL, not virtual: the kernel uses this value directly as the base
     * for its page tables. Passing the virtual form made TTBR0 come out as
     * 0xc07dc018 and the MMU walk unmapped memory. */
    PUT32(0x10, top_of_kernel_data);
    /* Boot_Video: no framebuffer yet, but give it a sane 320x480 geometry
     * rather than zeros, which some early code divides by. */
    PUT32(0x14, want_fb ? fb_pa : 0u); /* v_baseAddr */
    PUT32(0x18, want_fb ? 1u : 0u);    /* v_display */
    PUT32(0x1c, FB_W * FB_BPP);        /* v_rowBytes */
    PUT32(0x20, FB_W);                 /* v_width    */
    PUT32(0x24, FB_H);                 /* v_height   */
    PUT32(0x28, FB_BPP * 8);           /* v_depth    */
    PUT32(0x2c, 0);                    /* machineType           */
    PUT32(0x30, dt_len ? (dt_pa - phys_base + virt_base) : 0);
    PUT32(0x34, dt_len);
    memcpy(ba + 0x38, cmdline, strlen(cmdline) < 255 ? strlen(cmdline) : 255);
#undef PUT16
#undef PUT32
    s5l8900_load(&mach, ba_pa, ba, sizeof ba);
    printf("framebuffer: pa 0x%08x  %ux%u %u-bit\n", fb_pa, FB_W, FB_H, FB_BPP*8);
    printf("boot_args  : pa 0x%08x  topOfKernelData vm 0x%08x  cmdline \"%s\"\n\n",
           ba_pa, top_of_kernel_data - phys_base + virt_base, cmdline);

    mach.cpu.r[15] = entry_pa;
    mach.cpu.r[0]  = ba_pa;            /* XNU takes boot_args in r0 */

    /* Interpose on the bus so every non-RAM access is attributed to a PC. */
    spy_install(&mach, virt_base, phys_base);

    /*
     * Ring buffer of recent execution. When the kernel faults we want the
     * instructions that led there, not the state a few million instructions
     * later once it is spinning in a handler.
     */
#define KTRACE (1u << 18)
    static struct { uint32_t pc, cpsr, r[16]; } tr[KTRACE];
    unsigned tw = 0, tcount = 0;

    /*
     * Watchpoints on the failure machinery itself. panic() takes its printf
     * format in r0 and its arguments in r1..r3 then on the stack, so catching
     * the entry instruction is enough to recover the message the kernel was
     * trying to print — which is exactly what never reaches the UART.
     */
    struct wp { const char *name; uint32_t va; unsigned hits; };
    struct wp wps[] = {
        { "_panic",      ksym_value("_panic"),      0 },
        { "_Debugger",   ksym_value("_Debugger"),   0 },
        { "_kdb_printf", ksym_value("_kdb_printf"), 0 },
        { "_printf",     ksym_value("_printf"),     0 },
    };
    const unsigned nwps = (unsigned)(sizeof wps / sizeof wps[0]);
    for (unsigned i = 0; i < nwps; i++)
        printf("watch      : %-12s vm 0x%08x\n", wps[i].name, wps[i].va);
    printf("\n");

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

        /* How far down the console-init chain did we get? Each milestone is
         * matched at its virtual address and at its pre-MMU physical alias. */
        {
            uint32_t p = last_pc & ~1u;
            for (unsigned i = 0; i < NMILE; i++) {
                if (p != (MILESTONES[i].va & ~1u) && p != (G.mile_pa[i] & ~1u)) continue;
                if (!G.mile_hits[i]) G.mile_first[i] = n;
                G.mile_hits[i]++;
                break;
            }
        }

        /* Did we just land on one of the failure entry points? */
        for (unsigned w = 0; w < nwps; w++) {
            uint32_t va = wps[w].va;
            if (!va) continue;
            uint32_t pa = va - virt_base + phys_base;
            if ((last_pc & ~1u) != (va & ~1u) && (last_pc & ~1u) != (pa & ~1u)) continue;
            if (wps[w].hits++ >= 3) continue;    /* first few calls only */
            printf("=== %s entered (call #%u) at instruction %u ===\n",
                   wps[w].name, wps[w].hits, n);
            printf("  called from lr 0x%08x  (%s)\n",
                   mach.cpu.r[14], ksym_at(mach.cpu.r[14]));
            printf("  r0 %08x  r1 %08x  r2 %08x  r3 %08x  sp %08x\n",
                   mach.cpu.r[0], mach.cpu.r[1], mach.cpu.r[2],
                   mach.cpu.r[3], mach.cpu.r[13]);
            print_guest_str("format r0", mach.cpu.r[0]);
            /* varargs that are themselves strings are worth resolving too */
            uint32_t av[3] = { mach.cpu.r[1], mach.cpu.r[2], mach.cpu.r[3] };
            for (int a = 0; a < 3; a++) {
                const uint8_t *p = guest_ptr(av[a], 2);
                if (p && p[0] >= 0x20 && p[0] < 0x7f) {
                    char lbl[16]; snprintf(lbl, sizeof lbl, "arg r%d", a + 1);
                    print_guest_str(lbl, av[a]);
                }
            }
            /* Stack-passed varargs and the return chain live here. */
            const uint8_t *sp = guest_ptr(mach.cpu.r[13], 64);
            if (sp) {
                printf("  stack:");
                for (int k = 0; k < 8; k++) printf(" %08x", ld32(sp + k * 4));
                printf("\n");
            }
            /* Where did we come from? Show the distinct functions on the way. */
            printf("  recent call path (oldest first, one line per function):\n");
            unsigned start = (tw + KTRACE - tcount) % KTRACE;
            char seen[160]; seen[0] = '\0';
            for (unsigned i = tcount > 4096 ? tcount - 4096 : 0; i < tcount; i++) {
                unsigned k = (start + i) % KTRACE;
                const char *nm = ksym_at(tr[k].pc);
                char base[160];
                const char *bar = strchr(nm, '+');
                snprintf(base, sizeof base, "%.*s",
                         bar ? (int)(bar - nm) : (int)strlen(nm), nm);
                if (strcmp(base, seen)) {
                    printf("    %08x  %s\n", tr[k].pc, nm);
                    snprintf(seen, sizeof seen, "%s", base);
                }
            }
            printf("\n");
            fflush(stdout);
        }

        uint32_t mode_before = mach.cpu.cpsr & ARM_CPSR_MODE_MASK;
        uint32_t t_before    = mach.cpu.cpsr & ARM_CPSR_T;
        uint32_t lr_before   = mach.cpu.r[14];

        /* Pre-decode an ARM STREX* so its success/failure can be read out of
         * Rd after the step. Rd == 15 is unpredictable and never emitted. */
        unsigned strex_rd = 16, strex_rn = 16;
        if (!t_before) {
            const uint8_t *ip_ = guest_ptr(last_pc, 4);
            if (ip_) {
                uint32_t iw = ld32(ip_);
                uint32_t k  = iw & 0x0ff00ff0u;
                if ((k == 0x01800f90u || k == 0x01a00f90u ||
                     k == 0x01c00f90u || k == 0x01e00f90u) &&
                    (iw >> 28) != 0xfu) {
                    strex_rd = (iw >> 12) & 0xfu;
                    strex_rn = (iw >> 16) & 0xfu;
                }
            }
        }
        uint32_t strex_addr = strex_rn < 16 ? mach.cpu.r[strex_rn] : 0;

        st = arm_step(&mach.cpu);
        if (st != ARM_OK) break;
        s5l8900_tick(&mach, 1);

        if (strex_rd < 16 && mach.cpu.r[15] != last_pc) {
            G.strex_total++;
            if (mach.cpu.r[strex_rd] == 1u) {
                G.strex_failed++;
                if (G.strex_log_n < 32) {
                    G.strex_log[G.strex_log_n].at   = n;
                    G.strex_log[G.strex_log_n].pc   = last_pc;
                    G.strex_log[G.strex_log_n].addr = strex_addr;
                    G.strex_log_n++;
                }
            }
        }

        /* Exception return out of an ARM handler into Thumb code. */
        {
            uint32_t mode_after = mach.cpu.cpsr & ARM_CPSR_MODE_MASK;
            uint32_t t_after    = mach.cpu.cpsr & ARM_CPSR_T;
            if (mode_after != mode_before && t_after && !t_before) {
                G.exret_thumb++;
                if (!(mach.cpu.r[15] & 3u)) G.exret_thumb_aligned4++;
                /*
                 * Invariant: a handler returning into Thumb must resume at
                 * exactly lr (bit 0 cleared). Any mismatch means the core has
                 * aligned the resume address for the wrong instruction set —
                 * the defect that made a decrementer FIQ rewind Thumb code by
                 * two bytes and corrupt a zone free. This should stay zero.
                 */
                if ((lr_before & ~1u) != mach.cpu.r[15]) {
                    G.exret_mismatch++;
                    if (G.exret_log_n < 24) {
                        G.exret_log[G.exret_log_n].at        = n;
                        G.exret_log[G.exret_log_n].from_pc   = last_pc;
                        G.exret_log[G.exret_log_n].lr        = lr_before;
                        G.exret_log[G.exret_log_n].to_pc     = mach.cpu.r[15];
                        G.exret_log[G.exret_log_n].mode_from = mode_before;
                        G.exret_log[G.exret_log_n].mode_to   = mode_after;
                        G.exret_log_n++;
                    }
                }
            }
        }

        /* Report the first entry into an exception mode with the PC that caused
         * it — the kernel reading uninitialised per-CPU data usually means an
         * exception fired earlier than the kernel expected. */
        {
            uint32_t mode_after = mach.cpu.cpsr & ARM_CPSR_MODE_MASK;
            /* Record every distinct abort site. With the MMU on, an access to
             * an unmapped VIRTUAL address faults here and never reaches the
             * bus, so these are invisible in the unmapped-physical counters. */
            if (mode_after == ARM_MODE_ABT && mode_before != ARM_MODE_ABT) {
                bool pref = (mach.cpu.r[15] & 0xfffu) == 0x00cu;
                note_fault(pref ? mach.cpu.cp15.ifar : mach.cpu.cp15.dfar,
                           pref ? mach.cpu.cp15.ifsr : mach.cpu.cp15.dfsr,
                           last_pc, pref, n);
            }
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
        unsigned skip  = tcount > ktail ? tcount - ktail : 0;
        for (unsigned i = skip; i < tcount; i++) {
            unsigned k = (start + i) % KTRACE;
            /* absolute instruction index of this entry */
            unsigned idx = first_abort_at - (tcount - 1 - i);
            printf("    %-9u %08x %c m%02x r0=%08x r5=%08x r6=%08x "
                   "fp=%08x ip=%08x sp=%08x lr=%08x  %s\n",
                   idx, tr[k].pc,
                   (tr[k].cpsr & ARM_CPSR_T) ? 'T' : 'A',
                   tr[k].cpsr & ARM_CPSR_MODE_MASK,
                   tr[k].r[0], tr[k].r[5], tr[k].r[6], tr[k].r[11],
                   tr[k].r[12], tr[k].r[13], tr[k].r[14],
                   ksym_at(tr[k].pc >= phys_base && tr[k].pc < virt_base
                           ? tr[k].pc - phys_base + virt_base : tr[k].pc));
        }
        unsigned last = (tw + KTRACE - 1) % KTRACE;
        printf("\n  registers at the faulting instruction:\n");
        for (int i = 0; i < 16; i += 4)
            printf("    r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n",
                   i, tr[last].r[i], i+1, tr[last].r[i+1],
                   i+2, tr[last].r[i+2], i+3, tr[last].r[i+3]);
        printf("\n");
    }

    /* An abnormal stop (undefined instruction, halt) leaves no panic string,
     * so dump the same call path the panic watchpoints print — otherwise the
     * only evidence is a bare PC. */
    if (st != ARM_OK) {
        uint32_t p = last_pc;
        const uint8_t *ip = guest_ptr(p, 4);
        printf("\n=== ABNORMAL STOP: %s ===\n", status_name(st));
        if (ip) {
            if (mach.cpu.cpsr & ARM_CPSR_T)
                printf("  encoding at pc: %04x %04x (Thumb)\n",
                       (unsigned)(ip[0] | (ip[1] << 8)),
                       (unsigned)(ip[2] | (ip[3] << 8)));
            else
                printf("  encoding at pc: 0x%08x (ARM)\n", ld32(ip));
        }
        printf("  lr 0x%08x (%s)\n", mach.cpu.r[14], ksym_at(mach.cpu.r[14]));
        for (int i = 0; i < 16; i += 4)
            printf("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n",
                   i, mach.cpu.r[i], i+1, mach.cpu.r[i+1],
                   i+2, mach.cpu.r[i+2], i+3, mach.cpu.r[i+3]);
        printf("  recent call path (oldest first, one line per function):\n");
        unsigned start = (tw + KTRACE - tcount) % KTRACE;
        char seen[160]; seen[0] = '\0';
        for (unsigned i = tcount > 4096 ? tcount - 4096 : 0; i < tcount; i++) {
            unsigned k = (start + i) % KTRACE;
            uint32_t va = tr[k].pc >= phys_base && tr[k].pc < virt_base
                        ? tr[k].pc - phys_base + virt_base : tr[k].pc;
            const char *nm = ksym_at(va);
            char base[160];
            const char *bar = strchr(nm, '+');
            snprintf(base, sizeof base, "%.*s",
                     bar ? (int)(bar - nm) : (int)strlen(nm), nm);
            if (strcmp(base, seen)) {
                printf("    %08x  %s\n", tr[k].pc, nm);
                snprintf(seen, sizeof seen, "%s", base);
            }
        }
        printf("\n");
    }

    mach.uart0.tx[mach.uart0.tx_len] = '\0';
    printf("stopped after %u instructions: %s\n", n, status_name(st));
    printf("  pc             : 0x%08x", last_pc);
    if (last_pc >= phys_base && last_pc < phys_base + ram_size)
        printf("  (vm 0x%08x)", last_pc - phys_base + virt_base);
    printf("  %s\n", ksym_at(last_pc >= phys_base && last_pc < virt_base
                             ? last_pc - phys_base + virt_base : last_pc));
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

    /* ---------------- console-path diagnostics ---------------- */
    printf("\n=== UART PAGE (0x%08x) ACCESSES: %llu ===\n",
           UARTPG, (unsigned long long)G.uart_hits);
    for (unsigned i = 0; i < G.uart_log_n; i++)
        printf("    %s %s off 0x%02x  val 0x%08x  by pc 0x%08x %s\n",
               G.uart_log[i].wr ? "WRITE" : "READ ",
               G.uart_log[i].bytes == 4 ? "w" : G.uart_log[i].bytes == 2 ? "h" : "b",
               G.uart_log[i].addr - UARTPG, G.uart_log[i].val, G.uart_log[i].pc,
               ksym_at(G.uart_log[i].pc >= phys_base && G.uart_log[i].pc < virt_base
                       ? G.uart_log[i].pc - phys_base + virt_base : G.uart_log[i].pc));

    printf("\n=== ALL NON-RAM PHYSICAL PAGES TOUCHED (%u) ===\n", G.dev_page_n);
    for (unsigned i = 0; i < G.dev_page_n; i++)
        printf("    0x%08x  r=%-8llu w=%-8llu first pc 0x%08x %s\n",
               G.dev_page[i].page, (unsigned long long)G.dev_page[i].reads,
               (unsigned long long)G.dev_page[i].writes, G.dev_page[i].first_pc,
               ksym_at(G.dev_page[i].first_pc >= phys_base && G.dev_page[i].first_pc < virt_base
                       ? G.dev_page[i].first_pc - phys_base + virt_base
                       : G.dev_page[i].first_pc));

    printf("\n=== CONSOLE-INIT MILESTONES (xnu-1357.5.30 symbols) ===\n");
    printf("    --- NEVER REACHED ---\n");
    for (unsigned i = 0; i < NMILE; i++)
        if (!G.mile_hits[i])
            printf("    %-28s vm 0x%08x\n", MILESTONES[i].name, MILESTONES[i].va);
    printf("    --- reached, with call counts and first instruction index ---\n");
    for (unsigned i = 0; i < NMILE; i++)
        if (G.mile_hits[i])
            printf("    %-28s hits %-10llu first @ instr %u\n", MILESTONES[i].name,
                   (unsigned long long)G.mile_hits[i], G.mile_first[i]);

    printf("\n=== EXCEPTION RETURNS INTO THUMB ===\n");
    printf("    total                       : %llu\n", (unsigned long long)G.exret_thumb);
    printf("    resumed at a 4-aligned addr : %llu  (expect ~50%% on real hw)\n",
           (unsigned long long)G.exret_thumb_aligned4);
    printf("    resumed != lr (MOVS pc,lr)  : %llu\n", (unsigned long long)G.exret_mismatch);
    for (unsigned i = 0; i < G.exret_log_n; i++)
        printf("      @%-10u mode %02x->%02x  handler pc 0x%08x  lr 0x%08x -> resumed 0x%08x  %s\n",
               G.exret_log[i].at, G.exret_log[i].mode_from, G.exret_log[i].mode_to,
               G.exret_log[i].from_pc, G.exret_log[i].lr, G.exret_log[i].to_pc,
               ksym_at(G.exret_log[i].to_pc));

    printf("\n=== ARM STREX/STREXB/STREXH/STREXD ===\n");
    printf("    executed : %llu\n", (unsigned long long)G.strex_total);
    printf("    FAILED   : %llu\n", (unsigned long long)G.strex_failed);
    for (unsigned i = 0; i < G.strex_log_n; i++)
        printf("      @%-10u pc 0x%08x addr 0x%08x  %s\n",
               G.strex_log[i].at, G.strex_log[i].pc, G.strex_log[i].addr,
               ksym_at(G.strex_log[i].pc));

    printf("\n=== DISTINCT ABORT SITES (%u) ===\n", G.fault_n);
    for (unsigned i = 0; i < G.fault_n; i++)
        printf("    %s FAR 0x%08x FSR 0x%02x  pc 0x%08x %s  n=%llu first@%u\n",
               G.fault[i].prefetch ? "IFETCH" : "DATA  ",
               G.fault[i].far_, G.fault[i].fsr & 0xff, G.fault[i].pc,
               ksym_at(G.fault[i].pc >= phys_base && G.fault[i].pc < virt_base
                       ? G.fault[i].pc - phys_base + virt_base : G.fault[i].pc),
               (unsigned long long)G.fault[i].n, G.fault[i].first_at);

    if (mach.uart0.tx_len)
        printf("\n=== KERNEL UART OUTPUT (%zu bytes) ===\n%s\n",
               mach.uart0.tx_len, mach.uart0.tx);
    else
        printf("\n(no UART output yet)\n");

    /* Did the kernel actually draw? Count non-zero framebuffer bytes and, if
     * so, write a PPM so the console output can be looked at directly. */
    {
        uint32_t nonzero = 0;
        for (uint32_t i = 0; i < fb_bytes; i++)
            if (mach.ram[fb_pa - phys_base + i]) nonzero++;
        printf("\nframebuffer: %u of %u bytes non-zero\n", nonzero, fb_bytes);
        if (nonzero) {
            FILE *o = fopen("firmware/screen.ppm", "wb");
            if (o) {
                fprintf(o, "P6\n%u %u\n255\n", FB_W, FB_H);
                for (uint32_t y = 0; y < FB_H; y++)
                    for (uint32_t x = 0; x < FB_W; x++) {
                        const uint8_t *px =
                            &mach.ram[fb_pa - phys_base + (y * FB_W + x) * 4];
                        /* Stored BGRA; emit RGB. */
                        fputc(px[2], o); fputc(px[1], o); fputc(px[0], o);
                    }
                fclose(o);
                printf("wrote firmware/screen.ppm - THE KERNEL DREW SOMETHING\n");
            }
        }
    }

    s5l8900_free(&mach);
    free(img);
    return 0;
}

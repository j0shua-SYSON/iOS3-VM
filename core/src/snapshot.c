/*
 * iOS3-VM — full machine snapshot / restore.  See core/include/snapshot.h for
 * what this promises and why it exists.
 *
 * ===========================================================================
 * HOW TO ADD A FIELD  (read this before touching a device struct)
 * ===========================================================================
 * Every field of every struct is named EXACTLY ONCE in this file, inside a
 * "visitor" function — snap_cpu(), snap_uart(), snap_timer(), and so on. A
 * visitor does not know whether it is saving, loading, or merely measuring:
 * the F32()/F64()/FB()/FA32()/FBYTES() macros dispatch on io->mode. That is
 * deliberate. It means save and load cannot drift apart, because they are the
 * same list of fields read in the same order.
 *
 * So, to add a field to a device:
 *
 *   1. Add it to the struct in soc.h (or arm.h) as usual.
 *   2. Add ONE line to that struct's visitor below.
 *   3. Update the corresponding number in the STRUCT SIZE GUARDS block and
 *      bump SNAPSHOT_VERSION in snapshot.h.
 *
 * Step 3 is what makes step 2 hard to forget. The guards are _Static_asserts
 * on sizeof() of every struct that is snapshotted. Add a field and the build
 * BREAKS, with a message naming the visitor you have to extend. C gives no way
 * to enumerate a struct's members, so a size guard is the closest thing to
 * "you cannot compile a snapshot that has forgotten a register" — and a
 * forgotten register is a boot that diverges days later.
 *
 * A field that must NOT be snapshotted (a host pointer: m->ram, nor.data,
 * stub.regs, cpu.bus, the bus vtable) is still accounted for: it is listed in
 * the "deliberately not serialised" comment in its visitor. Silence about a
 * field is always a bug.
 *
 * ===========================================================================
 * FILE FORMAT
 * ===========================================================================
 *   header  (40 bytes, little-endian)
 *       0  char     magic[16]        "iOS3-VM SNAPSHOT"
 *      16  uint32   version          SNAPSHOT_VERSION, exact match required
 *      20  uint32   header_len       40
 *      24  uint64   payload_len
 *      32  uint64   flags            0
 *   payload (payload_len bytes)      a sequence of sections
 *   trailer (8 bytes)
 *       0  uint64   legacy FNV-1a-style hash over the payload bytes
 *
 *   section:  uint32 tag, uint32 zero, uint64 len, then len bytes
 *   sections, in order: GEOM CPU  MACH RAM  NOR  STUB END
 *
 * The checksum is a TRAILER rather than a header field on purpose: it means
 * the whole file can be written in a single forward pass with no seeking back
 * to patch a header, which keeps this portable to any sink (a file, a socket,
 * an iOS document) and keeps the writer trivial enough to be obviously right.
 * The payload length is known in advance because the visitors can be run in a
 * third "count" mode that measures instead of writing.
 *
 * ===========================================================================
 * RAM ENCODING — the tradeoff, stated
 * ===========================================================================
 * Guest RAM is up to 512 MB and a naive dump writes all of it every time. The
 * scheme here is the simplest one that is impossible to get wrong: RAM is cut
 * into 4 KB pages and every page is CLASSIFIED BY READING IT — 0 = all zero
 * (nothing stored), 1 = all bytes equal (one byte stored), 2 = anything else
 * (stored verbatim). There is no dirty-page tracking, no write barrier, no
 * cooperation required from the MMU or the bus, and therefore no way for a
 * missed write to silently corrupt a snapshot. The cost is one linear scan of
 * RAM per save, which is a few hundred milliseconds against a boot measured in
 * minutes.
 *
 * That was a deliberate choice over a dirty-bitmap scheme: a dirty bitmap
 * would make saves cheaper and would have to be maintained by every path that
 * writes guest memory (bus_write, s5l8900_load, DMA models yet to be written).
 * One forgotten path there produces exactly the class of silent divergence
 * this feature exists to prevent, and the saving is on the cheap side of the
 * loop. Simple and correct wins.
 *
 * With a 433 MB RAM disk resident in DRAM the classifier does not save much —
 * the file is roughly the size of the live data, as it must be. With a bare
 * kernel boot (no -r) most of DRAM is still zero and the file is small.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

/* ===========================================================================
 * STRUCT SIZE GUARDS — see "HOW TO ADD A FIELD" above.
 *
 * Only checked on a 64-bit host, because the numbers include the padding the
 * ABI chose and a 32-bit build lays these structs out differently. The guard
 * is a development aid for the machines this is developed on, not a portable
 * assertion about the format; the FORMAT is byte-exact everywhere because
 * every field is serialised with explicit little-endian primitives.
 * ======================================================================== */
#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 8
#define SNAP_SIZE_GUARD(type, bytes, visitor)                                  \
    _Static_assert(sizeof(type) == (bytes),                                    \
        #type " changed size. A snapshot that silently drops one register is " \
        "a boot that diverges days later, so this is a compile error on "      \
        "purpose: add the new field to " visitor "() in core/src/snapshot.c, " \
        "update this number, and bump SNAPSHOT_VERSION in snapshot.h.")

SNAP_SIZE_GUARD(arm_cp15_t,        64,    "snap_cpu");
SNAP_SIZE_GUARD(arm_cpu_t,         424,   "snap_cpu");
SNAP_SIZE_GUARD(s5l_uart_t,        8224,  "snap_uart");
SNAP_SIZE_GUARD(s5l_vic_t,         16,    "snap_vic");
SNAP_SIZE_GUARD(s5l_timer_t,       40,    "snap_timer");
SNAP_SIZE_GUARD(s5l_power_t,       24,    "snap_power");
SNAP_SIZE_GUARD(s5l_clcd_window_t, 24,    "snap_clcd");
SNAP_SIZE_GUARD(s5l_clcd_t,        3360,  "snap_clcd");
SNAP_SIZE_GUARD(s5l_nor_entry_t,   12,    "snap_nor");
SNAP_SIZE_GUARD(s5l_nor_t,         208,   "snap_nor");
SNAP_SIZE_GUARD(s5l_stub_t,        56,    "snap_stubs");
/* arm_bus_t's optional WFI platform callback is a host function pointer.  It
 * grows the containing machine ABI but is deliberately excluded from MACH for
 * the same reason as every other bus callback; snapshot_load preserves the
 * live machine's vtable.  The byte format therefore does not change. */
SNAP_SIZE_GUARD(s5l8900_t,         15776, "snap_mach");
#endif

/* ---------------------------------------------------------------- the IO --- */

#define TAG(a,b,c,d) (((uint32_t)(a)) | ((uint32_t)(b) << 8) | \
                      ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define TAG_GEOM TAG('G','E','O','M')
#define TAG_CPU  TAG('C','P','U',' ')
#define TAG_MACH TAG('M','A','C','H')
#define TAG_RAM  TAG('R','A','M',' ')
#define TAG_NOR  TAG('N','O','R',' ')
#define TAG_STUB TAG('S','T','U','B')
#define TAG_END  TAG('E','N','D',' ')

#define SNAP_HEADER_LEN 40u
#define SNAP_PAGE       4096u

typedef enum { SN_COUNT = 0, SN_SAVE, SN_LOAD, SN_VALIDATE } sn_mode_t;

typedef struct {
    sn_mode_t mode;
    /* sink for SN_SAVE: exactly one of f / (buf,cap) is used */
    FILE     *f;
    uint8_t  *buf;
    size_t    cap;
    /* source for SN_LOAD: exactly one of f / in */
    const uint8_t *in;
    size_t         in_len;
    uint64_t  pos;          /* bytes produced/consumed so far (all modes)   */
    uint64_t  hash;         /* legacy payload hash over bytes passed through */
    snapshot_status_t err;
} sn_io_t;

static bool sn_reading(const sn_io_t *io) {
    return io->mode == SN_LOAD || io->mode == SN_VALIDATE;
}

/* Historical v1/v2 FNV-1a-style hash. The offset below is not the standard
 * FNV-1a-64 basis; changing it would invalidate existing snapshots, so it is
 * deliberately retained and documented as format state. This is a corruption
 * check, not a security primitive. */
#define FNV64_OFFSET 1469598103934665603ull
#define FNV64_PRIME  1099511628211ull

static void sn_hash(sn_io_t *io, const uint8_t *p, size_t n) {
    uint64_t h = io->hash;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= FNV64_PRIME; }
    io->hash = h;
}

static void sn_raw(sn_io_t *io, void *p, size_t n) {
    if (io->err != SNAP_OK) return;
    switch (io->mode) {
        case SN_COUNT:
            break;
        case SN_SAVE:
            if (io->f) {
                if (fwrite(p, 1, n, io->f) != n) { io->err = SNAP_ERR_IO; return; }
            } else {
                if (io->pos > io->cap || n > io->cap - (size_t)io->pos) {
                    io->err = SNAP_ERR_NOMEM; return;
                }
                memcpy(io->buf + io->pos, p, n);
            }
            sn_hash(io, p, n);
            break;
        case SN_LOAD:
        case SN_VALIDATE:
            if (io->f) {
                if (fread(p, 1, n, io->f) != n) { io->err = SNAP_ERR_TRUNCATED; return; }
            } else {
                if (io->pos > io->in_len || n > io->in_len - (size_t)io->pos) {
                    io->err = SNAP_ERR_TRUNCATED; return;
                }
                memcpy(p, io->in + io->pos, n);
            }
            sn_hash(io, p, n);
            break;
    }
    io->pos += n;
}

/* Consume bytes without assigning them anywhere. Validation uses this for the
 * large host-owned RAM/NOR/stub buffers so a dry run cannot touch live state. */
static void sn_discard(sn_io_t *io, size_t n) {
    uint8_t scratch[4096];
    while (n && io->err == SNAP_OK) {
        size_t chunk = n < sizeof scratch ? n : sizeof scratch;
        sn_raw(io, scratch, chunk);
        n -= chunk;
    }
}

/* --- little-endian primitives. Every field goes through one of these, so the
 * file is identical on any host regardless of struct padding or byte order. */

static void sn_u8(sn_io_t *io, uint8_t *v) { sn_raw(io, v, 1); }

static void sn_u32(sn_io_t *io, uint32_t *v) {
    uint8_t b[4];
    if (io->mode == SN_SAVE) {
        b[0] = (uint8_t)*v; b[1] = (uint8_t)(*v >> 8);
        b[2] = (uint8_t)(*v >> 16); b[3] = (uint8_t)(*v >> 24);
    }
    sn_raw(io, b, 4);
    if (sn_reading(io) && io->err == SNAP_OK)
        *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
             ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void sn_u64(sn_io_t *io, uint64_t *v) {
    uint8_t b[8];
    if (io->mode == SN_SAVE)
        for (unsigned i = 0; i < 8; i++) b[i] = (uint8_t)(*v >> (8 * i));
    sn_raw(io, b, 8);
    if (sn_reading(io) && io->err == SNAP_OK) {
        uint64_t x = 0;
        for (unsigned i = 0; i < 8; i++) x |= (uint64_t)b[i] << (8 * i);
        *v = x;
    }
}

static void sn_bool(sn_io_t *io, bool *v) {
    uint8_t b = (io->mode == SN_SAVE) ? (*v ? 1u : 0u) : 0u;
    sn_u8(io, &b);
    if (sn_reading(io) && io->err == SNAP_OK) {
        if (b > 1u) io->err = SNAP_ERR_CORRUPT;
        else *v = b != 0;
    }
}

static void sn_u32a(sn_io_t *io, uint32_t *a, size_t n) {
    for (size_t i = 0; i < n; i++) sn_u32(io, &a[i]);
}

static void sn_size(sn_io_t *io, size_t *v) {   /* size_t as a fixed u64 */
    uint64_t x = (io->mode == SN_SAVE) ? (uint64_t)*v : 0;
    sn_u64(io, &x);
    if (sn_reading(io) && io->err == SNAP_OK) {
        if (x > (uint64_t)SIZE_MAX) io->err = SNAP_ERR_CORRUPT;
        else *v = (size_t)x;
    }
}

/* The field macros the visitors use. `io` is captured by name deliberately:
 * every visitor takes a parameter called `io`, so a field line is just the
 * field. */
#define F8(x)       sn_u8  (io, &(x))
#define F32(x)      sn_u32 (io, &(x))
#define F64(x)      sn_u64 (io, &(x))
#define FB(x)       sn_bool(io, &(x))
#define FSZ(x)      sn_size(io, &(x))
#define FA32(a, n)  sn_u32a(io, (a), (n))
#define FBYTES(p,n) sn_raw (io, (p), (n))

/* ===========================================================================
 * THE VISITORS.  One per struct. Every mutable field appears exactly once.
 * ======================================================================== */

static void snap_cp15(sn_io_t *io, arm_cp15_t *p) {
    F32(p->sctlr);      F32(p->actlr);      F32(p->cpacr);
    F32(p->ttbr0);      F32(p->ttbr1);      F32(p->ttbcr);
    F32(p->dacr);
    F32(p->dfsr);       F32(p->ifsr);
    F32(p->dfar);       F32(p->ifar);
    F32(p->fcse_pid);   F32(p->context_id);
    F32(p->tpidrurw);   F32(p->tpidruro);   F32(p->tpidrprw);
}

/*
 * The CPU. This list is the reset path in arm_interp.c (arm_reset) read field
 * by field; if a future register is added there it must be added here, and the
 * size guard above will insist.
 *
 * Deliberately NOT serialised: `bus`, a host pointer into the machine struct.
 * snapshot_load re-points it at the live machine's bus, which is what lets a
 * tool wrap the bus callbacks and still restore underneath the wrapper.
 */
static void snap_cpu(sn_io_t *io, arm_cpu_t *c) {
    FA32(c->r, 16);
    F32(c->cpsr);
    snap_cp15(io, &c->cp15);
    FA32(c->spsr,      ARM_BANK_COUNT);
    FA32(c->bank_r13,  ARM_BANK_COUNT);
    FA32(c->bank_r14,  ARM_BANK_COUNT);
    FA32(c->fiq_r8_12, 5);
    FA32(c->usr_r8_12, 5);
    F64(c->cycles);
    FB (c->abort_pending);
    F32(c->abort_fsr);
    F32(c->abort_far);
    FB (c->irq_line);
    FB (c->fiq_line);
    FB (c->excl_valid);
    F32(c->excl_addr);
    F32(c->vfp_fpexc);
    F32(c->vfp_fpscr);
    /* s0-s31. d0-d15 alias these and so need no separate entry — that is the
     * point of storing the file once (see arm_cpu_t.vfp_s). */
    FA32(c->vfp_s, 32);
}

/* The UART's whole transmit capture is saved, not just its used prefix: the
 * buffer is 8 KB, the simplicity is worth more than the bytes, and a restored
 * machine then compares byte-identical to the original including the slack. */
static void snap_uart(sn_io_t *io, s5l_uart_t *u) {
    F32(u->ulcon); F32(u->ucon); F32(u->ufcon); F32(u->umcon); F32(u->ubrdiv);
    FBYTES(u->tx, UART_TX_BUFFER);
    FSZ(u->tx_len);
    if (sn_reading(io) && io->err == SNAP_OK &&
        u->tx_len >= (size_t)UART_TX_BUFFER)
        io->err = SNAP_ERR_CORRUPT;     /* the writer keeps a NUL slot free */
}

static void snap_vic(sn_io_t *io, s5l_vic_t *v) {
    F32(v->raw); F32(v->enable); F32(v->select); F32(v->soft);
}

static void snap_timer(sn_io_t *io, s5l_timer_t *t) {
    F64(t->ticks);
    F32(t->config);
    F32(t->t4_config); F32(t->t4_state);
    F32(t->t4_count);  F32(t->t4_count2); F32(t->t4_value);
    F32(t->irqlatch);
}

static void snap_power(sn_io_t *io, s5l_power_t *p) {
    F32(p->state); F32(p->cfg0); F32(p->cfg1);
    F32(p->sram);  F32(p->cfg24); F32(p->cfg28);
}

static void snap_clcd(sn_io_t *io, s5l_clcd_t *c) {
    F32(c->enable); F32(c->disable); F32(c->ctrl); F32(c->fifo);
    F32(c->intmask); F32(c->intstatus); F32(c->reg1c);
    F32(c->preenable); F32(c->backdrop);
    FA32(c->video, sizeof c->video / sizeof c->video[0]);
    for (unsigned k = 0; k < CLCD_WIN_COUNT; k++) {
        s5l_clcd_window_t *w = &c->win[k];
        F32(w->stride); F32(w->control); F32(w->fbaddr);
        F32(w->geometry); F32(w->linewords); F32(w->position);
    }
    F32(c->update); F32(c->update2);
    FA32(c->timing, sizeof c->timing / sizeof c->timing[0]);
    FA32(c->csc,    sizeof c->csc    / sizeof c->csc[0]);
    F32(c->gate);
    FA32(c->opaque, sizeof c->opaque / sizeof c->opaque[0]);
    FA32(&c->gamma[0][0], 3u * 256u);
    FB (c->scanning);
    F32(c->frame_ticks);
    F32(c->frame_accum);
    F64(c->frames);
}

/*
 * NOR. The contents are saved as well as the scanned directory: a guest
 * payload can program the flash (that is how an untethered jailbreak persists
 * itself), so the NOR is mutable guest-visible state, not a constant.
 *
 * Deliberately NOT serialised: `data`, the host allocation. Its SIZE is
 * checked against the live machine in the GEOM section before anything is
 * touched, and the bytes are then written through the existing pointer.
 */
static void snap_nor(sn_io_t *io, s5l_nor_t *n) {
    if (io->mode == SN_VALIDATE) sn_discard(io, n->size);
    else                         FBYTES(n->data, n->size);
    for (unsigned i = 0; i < S5L_NOR_MAX_IMAGES; i++) {
        F32(n->images[i].ident);
        F32(n->images[i].offset);
        F32(n->images[i].size);
    }
    F32(n->image_count);
    if (sn_reading(io) && io->err == SNAP_OK &&
        n->image_count > (unsigned)S5L_NOR_MAX_IMAGES)
        io->err = SNAP_ERR_CORRUPT;
    if (sn_reading(io) && io->err == SNAP_OK) {
        for (unsigned i = 0; i < n->image_count; i++) {
            if (n->images[i].size < 20u ||
                (uint64_t)n->images[i].offset + n->images[i].size > n->size) {
                io->err = SNAP_ERR_CORRUPT;
                break;
            }
        }
    }
}

/*
 * The machine's own state, excluding RAM, NOR, the stub backing stores and the
 * CPU (each of which has its own section).
 *
 * Deliberately NOT serialised: `cpu` (own section), `bus` (host function
 * pointers — a tool may have interposed on them), `ram` (host allocation),
 * `nor.data` and `stubs[].regs`/`stubs[].name` (host allocations / string
 * literals). ram_base/ram_size live in GEOM.
 */
static void snap_mach(sn_io_t *io, s5l8900_t *m) {
    snap_uart(io, &m->uart0);
    for (unsigned i = 0; i < S5L8900_VIC_COUNT; i++) snap_vic(io, &m->vic[i]);
    snap_timer(io, &m->timer);
    snap_power(io, &m->power);
    snap_clcd (io, &m->clcd);

    F64(m->unmapped_reads);
    F64(m->unmapped_writes);
    FA32(m->unmapped_addr, S5L_UNMAPPED_LOG);
    F32(m->unmapped_addr_count);

    FB (m->trace_devices);
    FA32(m->dev_addr,  S5L_DEVLOG);
    FA32(m->dev_value, S5L_DEVLOG);
    for (unsigned i = 0; i < S5L_DEVLOG; i++) FB(m->dev_is_write[i]);
    F32(m->dev_count);

    F32(m->cpu_hz);
    F32(m->tb_hz);
    F64(m->tb_accum);

    F32(m->stub_declare_failures);

    if (sn_reading(io) && io->err == SNAP_OK &&
        (m->unmapped_addr_count > (unsigned)S5L_UNMAPPED_LOG ||
         m->dev_count           > (unsigned)S5L_DEVLOG))
        io->err = SNAP_ERR_CORRUPT;
}

/* ------------------------------------------------------------ sections --- */

/*
 * GEOM: everything that describes the SHAPE of the machine rather than its
 * contents. It is the first section so that a mismatch is caught before a
 * single byte of the live machine has been modified.
 */
static void snap_geom(sn_io_t *io, s5l8900_t *m) {
    uint32_t ram_base = m->ram_base, ram_size = m->ram_size;
    uint32_t nor_size = m->nor.size, stub_count = m->stub_count;
    uint32_t page = SNAP_PAGE;
    F32(ram_base); F32(ram_size); F32(nor_size); F32(stub_count); F32(page);

    if (sn_reading(io)) {
        if (io->err != SNAP_OK) return;
        if (ram_base != m->ram_base || ram_size != m->ram_size ||
            nor_size != m->nor.size || stub_count != m->stub_count ||
            page != SNAP_PAGE) {
            io->err = SNAP_ERR_GEOMETRY;
            return;
        }
    }

    /* Each stub window is identified by base/size/name so that a snapshot
     * taken before a peripheral window was added, renamed, or resized is
     * refused rather than restored into the wrong backing store. */
    for (unsigned i = 0; i < m->stub_count && io->err == SNAP_OK; i++) {
        uint32_t base = m->stubs[i].base, size = m->stubs[i].size;
        uint32_t nregs = m->stubs[i].nregs;
        const char *live = m->stubs[i].name ? m->stubs[i].name : "";
        uint32_t nlen = (uint32_t)strlen(live);
        char nm[64];
        if (io->mode == SN_SAVE) snprintf(nm, sizeof nm, "%s", live);
        else                     memset(nm, 0, sizeof nm);
        F32(base); F32(size); F32(nregs); F32(nlen);
        if (io->err == SNAP_OK && nlen >= sizeof nm) { io->err = SNAP_ERR_CORRUPT; return; }
        FBYTES(nm, nlen);
        if (sn_reading(io) && io->err == SNAP_OK) {
            nm[nlen] = '\0';
            if (base != m->stubs[i].base || size != m->stubs[i].size ||
                nregs != m->stubs[i].nregs || strcmp(nm, live) != 0) {
                io->err = SNAP_ERR_GEOMETRY;
                return;
            }
        }
    }
}

static void snap_cpu_sec(sn_io_t *io, s5l8900_t *m) { snap_cpu(io, &m->cpu); }
static void snap_nor_sec(sn_io_t *io, s5l8900_t *m) { snap_nor(io, &m->nor); }

/*
 * RAM, page-classified. See the header comment for why classification is a
 * read-only scan rather than a dirty bitmap.
 *
 *   uint64 npages
 *   uint8  class[npages]     0 = all zero, 1 = all one byte value, 2 = raw
 *   then, in page order: class 1 -> one byte, class 2 -> the page verbatim
 */
static void snap_ram(sn_io_t *io, s5l8900_t *m) {
    uint64_t npages = ((uint64_t)m->ram_size + SNAP_PAGE - 1u) / SNAP_PAGE;
    uint64_t n = npages;
    F64(n);
    if (io->err != SNAP_OK) return;
    if (sn_reading(io) && n != npages) { io->err = SNAP_ERR_CORRUPT; return; }

    if (io->mode == SN_LOAD) memset(m->ram, 0, m->ram_size);

    for (uint64_t p = 0; p < npages; p++) {
        uint64_t off = p * SNAP_PAGE;
        size_t   len = (size_t)((m->ram_size - off < SNAP_PAGE)
                                ? m->ram_size - off : SNAP_PAGE);
        uint8_t *page = m->ram + off;
        uint8_t  cls = 0;
        if (!sn_reading(io)) {
            uint8_t first = page[0];
            cls = 1;
            for (size_t i = 1; i < len; i++)
                if (page[i] != first) { cls = 2; break; }
            if (cls == 1 && first == 0) cls = 0;
        }
        sn_u8(io, &cls);
        if (io->err != SNAP_OK) return;
        if (cls == 0) continue;
        if (cls == 1) {
            uint8_t v = page[0];
            sn_u8(io, &v);
            if (io->mode == SN_LOAD && io->err == SNAP_OK) memset(page, v, len);
        } else if (cls == 2) {
            if (io->mode == SN_VALIDATE) sn_discard(io, len);
            else                         sn_raw(io, page, len);
        } else {
            io->err = SNAP_ERR_CORRUPT;
            return;
        }
    }
}

/* Stub backing stores and their access counters. The identity of each window
 * was already checked in GEOM, so only contents travel here. */
static void snap_stubs(sn_io_t *io, s5l8900_t *m) {
    for (unsigned i = 0; i < m->stub_count; i++) {
        s5l_stub_t *s = &m->stubs[i];
        if (io->mode == SN_VALIDATE) {
            for (uint32_t k = 0; k < s->nregs; k++) {
                uint32_t discard = 0;
                sn_u32(io, &discard);
            }
        } else {
            FA32(s->regs, s->nregs);
        }
        F64(s->reads); F64(s->writes); F64(s->oob);
    }
}

/* --------------------------------------------------------- the payload --- */

typedef void (*snap_fn_t)(sn_io_t *, s5l8900_t *);

static uint64_t snap_measure(snap_fn_t fn, s5l8900_t *m) {
    sn_io_t c = {0};
    c.mode = SN_COUNT;
    fn(&c, m);
    return c.pos;
}

static void snap_section(sn_io_t *io, uint32_t tag, snap_fn_t fn, s5l8900_t *m) {
    if (io->err != SNAP_OK) return;
    uint32_t t = tag, zero = 0;
    uint64_t len = sn_reading(io) ? 0 : snap_measure(fn, m);

    if (io->mode == SN_COUNT) { io->pos += 16 + len; return; }

    sn_u32(io, &t); sn_u32(io, &zero); sn_u64(io, &len);
    if (io->err != SNAP_OK) return;
    if (sn_reading(io)) {
        if (t != tag || zero != 0) { io->err = SNAP_ERR_CORRUPT; return; }
    }
    uint64_t start = io->pos;
    fn(io, m);
    if (io->err != SNAP_OK) return;
    if (io->pos - start != len) io->err = SNAP_ERR_CORRUPT;
}

static void snap_end(sn_io_t *io, s5l8900_t *m) { (void)io; (void)m; }

static void snap_payload(sn_io_t *io, s5l8900_t *m) {
    snap_section(io, TAG_GEOM, snap_geom,    m);
    snap_section(io, TAG_CPU,  snap_cpu_sec, m);
    snap_section(io, TAG_MACH, snap_mach,    m);
    snap_section(io, TAG_RAM,  snap_ram,     m);
    snap_section(io, TAG_NOR,  snap_nor_sec, m);
    snap_section(io, TAG_STUB, snap_stubs,   m);
    snap_section(io, TAG_END,  snap_end,     m);
}

static void snap_header(sn_io_t *io, uint64_t *payload_len) {
    char     magic[SNAPSHOT_MAGIC_LEN];
    uint32_t version = SNAPSHOT_VERSION, hlen = SNAP_HEADER_LEN;
    uint64_t flags = 0;

    if (io->mode == SN_SAVE) memcpy(magic, SNAPSHOT_MAGIC, SNAPSHOT_MAGIC_LEN);
    else                     memset(magic, 0, sizeof magic);

    /* The header is outside the checksummed payload, so it is written with the
     * raw primitives but must not disturb the running hash. */
    uint64_t saved_hash = io->hash;
    sn_raw(io, magic, SNAPSHOT_MAGIC_LEN);
    sn_u32(io, &version);
    sn_u32(io, &hlen);
    sn_u64(io, payload_len);
    sn_u64(io, &flags);
    io->hash = saved_hash;

    if (sn_reading(io) && io->err == SNAP_OK) {
        if (memcmp(magic, SNAPSHOT_MAGIC, SNAPSHOT_MAGIC_LEN) != 0)
            io->err = SNAP_ERR_MAGIC;
        else if (version != SNAPSHOT_VERSION)
            io->err = SNAP_ERR_VERSION;
        else if (hlen != SNAP_HEADER_LEN)
            io->err = SNAP_ERR_CORRUPT;
        else if (flags != 0)
            io->err = SNAP_ERR_CORRUPT;
    }
}

/* ------------------------------------------------------------ save side --- */

static snapshot_status_t snap_write(const s5l8900_t *cm, FILE *f,
                                    uint8_t *buf, size_t cap, size_t *written) {
    /* Casting away const is safe and confined: the visitors take a mutable
     * pointer because the SAME code loads, and in SN_SAVE/SN_COUNT mode not
     * one of them assigns through it. */
    s5l8900_t *m = (s5l8900_t *)(uintptr_t)cm;

    uint64_t payload_len = snap_measure(snap_payload, m);

    sn_io_t io = {0};
    io.mode = SN_SAVE; io.f = f; io.buf = buf; io.cap = cap;
    io.hash = FNV64_OFFSET;

    snap_header(&io, &payload_len);
    uint64_t body_start = io.pos;
    snap_payload(&io, m);
    if (io.err == SNAP_OK && io.pos - body_start != payload_len)
        io.err = SNAP_ERR_CORRUPT;

    /* Trailer: the checksum of everything after the header. */
    uint64_t h = io.hash, saved = io.hash;
    sn_u64(&io, &h);
    io.hash = saved;

    if (written) *written = (size_t)io.pos;
    return io.err;
}

static bool snap_machine_valid(const s5l8900_t *m) {
    if (!m || !m->ram || !m->ram_size || !m->nor.data || !m->nor.size ||
        m->stub_count > S5L_STUB_MAX ||
        m->uart0.tx_len >= UART_TX_BUFFER ||
        m->nor.image_count > S5L_NOR_MAX_IMAGES ||
        m->unmapped_addr_count > S5L_UNMAPPED_LOG || m->dev_count > S5L_DEVLOG)
        return false;
    for (unsigned i = 0; i < m->nor.image_count; i++)
        if (m->nor.images[i].size < 20u ||
            (uint64_t)m->nor.images[i].offset + m->nor.images[i].size > m->nor.size)
            return false;
    for (unsigned i = 0; i < m->stub_count; i++) {
        const s5l_stub_t *s = &m->stubs[i];
        uint64_t want = ((uint64_t)s->size + 3u) / 4u;
        const char *name = s->name ? s->name : "";
        if (!s->size || !s->regs || s->nregs != want || strlen(name) >= 64u)
            return false;
    }
    return true;
}

static char *snapshot_temp_path(const char *path, const void *tag) {
    size_t n = strlen(path);
    if (n > SIZE_MAX - 40u) return NULL;
    char *tmp = malloc(n + 40u);
    if (!tmp) return NULL;
    snprintf(tmp, n + 40u, "%s.tmp.%p", path, tag);
    return tmp;
}

static bool snapshot_replace_file(const char *tmp, const char *path) {
#ifdef _WIN32
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING |
                                  MOVEFILE_WRITE_THROUGH) != 0;
#else
    return rename(tmp, path) == 0;
#endif
}

snapshot_status_t snapshot_save(const s5l8900_t *m, const char *path) {
    if (!m || !path) return SNAP_ERR_IO;
    if (!snap_machine_valid(m)) return SNAP_ERR_CORRUPT;
    char *tmp = snapshot_temp_path(path, m);
    if (!tmp) return SNAP_ERR_NOMEM;
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(tmp); return SNAP_ERR_IO; }
    snapshot_status_t st = snap_write(m, f, NULL, 0, NULL);
    if (fclose(f) != 0 && st == SNAP_OK) st = SNAP_ERR_IO;
    if (st == SNAP_OK && !snapshot_replace_file(tmp, path)) st = SNAP_ERR_IO;
    if (st != SNAP_OK) remove(tmp);
    free(tmp);
    return st;
}

snapshot_status_t snapshot_save_mem(const s5l8900_t *m,
                                    uint8_t **out, size_t *out_len) {
    if (!out || !out_len) return SNAP_ERR_IO;
    *out = NULL; *out_len = 0;
    if (!m) return SNAP_ERR_IO;
    if (!snap_machine_valid(m)) return SNAP_ERR_CORRUPT;
    s5l8900_t *mm = (s5l8900_t *)(uintptr_t)m;
    uint64_t payload_len = snap_measure(snap_payload, mm);
    uint64_t total = SNAP_HEADER_LEN + payload_len + 8u;
    if (total > (uint64_t)(size_t)-1) return SNAP_ERR_NOMEM;
    uint8_t *buf = malloc((size_t)total);
    if (!buf) return SNAP_ERR_NOMEM;
    size_t written = 0;
    snapshot_status_t st = snap_write(m, NULL, buf, (size_t)total, &written);
    if (st != SNAP_OK || written != (size_t)total) {
        free(buf);
        return st == SNAP_OK ? SNAP_ERR_CORRUPT : st;
    }
    *out = buf; *out_len = written;
    return SNAP_OK;
}

/* ------------------------------------------------------------ load side --- */

/*
 * Loading has three stages. The first verifies framing and the legacy payload
 * hash without touching the machine. The second parses the complete stream
 * into a shallow probe in SN_VALIDATE mode: it checks section lengths,
 * geometry, scalar encodings, NOR metadata and the exact END position while
 * discarding all bulk contents. Only a stream that passes both stages reaches
 * the final applying pass.
 *
 * In-memory loads are therefore transactional for malformed input. A file
 * load can still be interrupted by a genuine read error (or external in-place
 * modification) during the final applying pass; callers must treat that
 * SNAP_ERR_IO/SNAP_ERR_TRUNCATED result as potentially partially applied.
 */
static snapshot_status_t snap_verify_stream(FILE *f, const uint8_t *in,
                                            size_t in_len, uint64_t *payload_len) {
    sn_io_t io = {0};
    io.mode = SN_LOAD; io.f = f; io.in = in; io.in_len = in_len;
    io.hash = FNV64_OFFSET;

    uint64_t plen = 0;
    snap_header(&io, &plen);
    if (io.err != SNAP_OK) return io.err;

    if (plen > UINT64_MAX - SNAP_HEADER_LEN - 8u) return SNAP_ERR_CORRUPT;
    uint64_t total = SNAP_HEADER_LEN + plen + 8u;

    /* A declared length that cannot fit is a truncated file, not a huge one. */
    if (!f && (uint64_t)in_len < total)
        return SNAP_ERR_TRUNCATED;

    uint8_t chunk[65536];
    uint64_t left = plen;
    while (left) {
        size_t want = left > sizeof chunk ? sizeof chunk : (size_t)left;
        sn_raw(&io, chunk, want);
        if (io.err != SNAP_OK) return io.err;
        left -= want;
    }
    uint64_t recorded = 0, computed = io.hash;
    uint64_t saved = io.hash;
    sn_u64(&io, &recorded);
    io.hash = saved;
    if (io.err != SNAP_OK) return io.err;
    if (recorded != computed) return SNAP_ERR_CHECKSUM;

    /* Trailing junk is a sign the file is not what it claims to be. */
    if (!f && (uint64_t)in_len != total)
        return SNAP_ERR_CORRUPT;
    if (f) {
        uint8_t extra;
        if (fread(&extra, 1, 1, f) == 1) return SNAP_ERR_CORRUPT;
        if (ferror(f)) return SNAP_ERR_IO;
    }
    *payload_len = plen;
    return SNAP_OK;
}

/* Parse every section and validate every constrained field into a shallow
 * machine copy. Large host-owned buffers are consumed with sn_discard(), so
 * this pass cannot modify the live machine. A checksummed but structurally
 * hostile stream is therefore rejected before the applying pass starts. */
static snapshot_status_t snap_validate_structure(const s5l8900_t *m, FILE *f,
                                                  const uint8_t *in,
                                                  size_t in_len) {
    s5l8900_t probe = *m;
    sn_io_t io = {0};
    io.mode = SN_VALIDATE; io.f = f; io.in = in; io.in_len = in_len;
    io.hash = FNV64_OFFSET;

    uint64_t plen = 0;
    snap_header(&io, &plen);
    if (io.err != SNAP_OK) return io.err;
    uint64_t body_start = io.pos;
    snap_payload(&io, &probe);
    if (io.err != SNAP_OK) return io.err;
    if (io.pos - body_start != plen) return SNAP_ERR_CORRUPT;
    return SNAP_OK;
}

static snapshot_status_t snap_apply(s5l8900_t *m, FILE *f,
                                    const uint8_t *in, size_t in_len) {
    sn_io_t io = {0};
    io.mode = SN_LOAD; io.f = f; io.in = in; io.in_len = in_len;
    io.hash = FNV64_OFFSET;

    uint64_t plen = 0;
    snap_header(&io, &plen);
    if (io.err != SNAP_OK) return io.err;
    uint64_t body_start = io.pos;
    snap_payload(&io, m);
    if (io.err != SNAP_OK) return io.err;
    if (io.pos - body_start != plen) return SNAP_ERR_CORRUPT;

    /* Host-owned wiring the visitors deliberately never touched. Re-pointing
     * the CPU at the live machine's bus is what makes restoring underneath an
     * interposed bus (bootkernel's tracing wrapper) work. */
    m->cpu.bus = &m->bus;
    m->bus.ctx = m;
    return SNAP_OK;
}

snapshot_status_t snapshot_load(s5l8900_t *m, const char *path) {
    if (!m || !path) return SNAP_ERR_IO;
    if (!snap_machine_valid(m)) return SNAP_ERR_GEOMETRY;
    FILE *f = fopen(path, "rb");
    if (!f) return SNAP_ERR_IO;

    uint64_t plen = 0;
    snapshot_status_t st = snap_verify_stream(f, NULL, 0, &plen);
    if (st != SNAP_OK) { fclose(f); return st; }

    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return SNAP_ERR_IO; }
    st = snap_validate_structure(m, f, NULL, 0);
    if (st != SNAP_OK) { fclose(f); return st; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return SNAP_ERR_IO; }
    st = snap_apply(m, f, NULL, 0);
    fclose(f);
    return st;
}

snapshot_status_t snapshot_load_mem(s5l8900_t *m, const uint8_t *buf, size_t len) {
    if (!m || !buf) return SNAP_ERR_IO;
    if (!snap_machine_valid(m)) return SNAP_ERR_GEOMETRY;
    uint64_t plen = 0;
    snapshot_status_t st = snap_verify_stream(NULL, buf, len, &plen);
    if (st != SNAP_OK) return st;
    st = snap_validate_structure(m, NULL, buf, len);
    if (st != SNAP_OK) return st;
    return snap_apply(m, NULL, buf, len);
}

const char *snapshot_strerror(snapshot_status_t st) {
    switch (st) {
        case SNAP_OK:            return "ok";
        case SNAP_ERR_IO:        return "I/O error";
        case SNAP_ERR_MAGIC:     return "not an iOS3-VM snapshot (bad magic)";
        case SNAP_ERR_VERSION:   return "snapshot format version mismatch";
        case SNAP_ERR_TRUNCATED: return "snapshot file is truncated";
        case SNAP_ERR_CHECKSUM:  return "snapshot payload failed its checksum";
        case SNAP_ERR_CORRUPT:   return "snapshot is structurally corrupt";
        case SNAP_ERR_GEOMETRY:  return "snapshot machine layout does not match "
                                        "this machine (RAM size/base, NOR size, "
                                        "or the stub windows differ)";
        case SNAP_ERR_NOMEM:     return "out of memory";
        default:                 return "unknown snapshot error";
    }
}

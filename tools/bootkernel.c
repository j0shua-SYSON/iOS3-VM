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
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "ksyms.h"
#include "macho.h"
#include "snapshot.h"
#include "soc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

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

/* Size and stream a large input without first mirroring it in a host buffer.
 * The root filesystem is hundreds of MiB, while the final copy already lives
 * in guest DRAM.  Keeping both copies almost doubles boot-time memory use.
 *
 * Keep the same open file from sizing through the final read.  The metadata
 * stamp cannot make a mutable file into a snapshot, but it catches ordinary
 * replacement and concurrent writes without allocating another image-sized
 * buffer. Size and second-resolution change times are common everywhere;
 * POSIX also gives a stable device/inode identity for the path check. */
#ifdef _WIN32
typedef struct _stat64 host_file_stat_t;
static int host_file_fstat(FILE *f, host_file_stat_t *st) {
    int fd = _fileno(f);
    return fd < 0 ? -1 : _fstat64(fd, st);
}
static int host_file_stat(const char *path, host_file_stat_t *st) {
    return _stat64(path, st);
}
#else
typedef struct stat host_file_stat_t;
static int host_file_fstat(FILE *f, host_file_stat_t *st) {
    int fd = fileno(f);
    return fd < 0 ? -1 : fstat(fd, st);
}
static int host_file_stat(const char *path, host_file_stat_t *st) {
    return stat(path, st);
}
#endif

typedef struct {
    FILE             *file;
    const char       *path;
    host_file_stat_t  stamp;
    size_t            size;
} streamed_file_t;

static bool host_file_stat_equal(const host_file_stat_t *a,
                                 const host_file_stat_t *b) {
    bool same = a->st_mode  == b->st_mode  && a->st_size  == b->st_size &&
                a->st_mtime == b->st_mtime && a->st_ctime == b->st_ctime;
#ifndef _WIN32
    same = same && a->st_dev == b->st_dev && a->st_ino == b->st_ino;
#endif
    return same;
}

static bool streamed_file_verify(const streamed_file_t *source,
                                 const char *when) {
    host_file_stat_t opened_now, path_now;
    if (!source || !source->file || !source->path) return false;
    if (host_file_fstat(source->file, &opened_now) != 0 ||
        host_file_stat(source->path, &path_now) != 0) {
        perror("stat");
        return false;
    }
    if (!host_file_stat_equal(&source->stamp, &opened_now) ||
        !host_file_stat_equal(&source->stamp, &path_now)) {
        fprintf(stderr, "read: input changed %s\n", when);
        return false;
    }
    return true;
}

static bool streamed_file_open(const char *path, streamed_file_t *source,
                               size_t *len_out) {
    host_file_stat_t path_stamp;
    if (!path || !source || !len_out) return false;
    memset(source, 0, sizeof *source);
    source->file = fopen(path, "rb");
    if (!source->file) { perror("open"); return false; }
    source->path = path;
    if (host_file_fstat(source->file, &source->stamp) != 0 ||
        host_file_stat(path, &path_stamp) != 0) {
        perror("stat");
        fclose(source->file);
        source->file = NULL;
        return false;
    }
    if (!host_file_stat_equal(&source->stamp, &path_stamp)) {
        fprintf(stderr, "open: input changed while it was being opened\n");
        fclose(source->file);
        source->file = NULL;
        return false;
    }
    if (source->stamp.st_size < 0 ||
        (uintmax_t)source->stamp.st_size > (uintmax_t)SIZE_MAX) {
        fprintf(stderr, "open: input is too large for this host\n");
        fclose(source->file);
        source->file = NULL;
        return false;
    }
    clearerr(source->file);
    if (fseek(source->file, 0, SEEK_SET) != 0) {
        perror("seek");
        fclose(source->file);
        source->file = NULL;
        return false;
    }
    source->size = (size_t)source->stamp.st_size;
    *len_out = source->size;
    return true;
}

static bool streamed_file_close(streamed_file_t *source) {
    if (!source || !source->file) return true;
    FILE *f = source->file;
    source->file = NULL;
    if (fclose(f) != 0) {
        perror("close");
        return false;
    }
    return true;
}

static bool stream_file(streamed_file_t *source, uint8_t *dst,
                        size_t expected) {
    if (!source || !source->file || expected != source->size ||
        (!dst && expected)) return false;
    if (!streamed_file_verify(source, "before it was loaded")) return false;
    FILE *f = source->file;

    size_t done = 0;
    while (done < expected) {
        size_t chunk = expected - done;
        if (chunk > (1u << 20)) chunk = 1u << 20;
        size_t got = fread(dst + done, 1, chunk, f);
        if (!got) {
            if (ferror(f)) {
                int saved_errno = errno;
                fprintf(stderr, "read: failed after %llu of %llu bytes: ",
                        (unsigned long long)done,
                        (unsigned long long)expected);
                fprintf(stderr, "%s\n", strerror(saved_errno));
            }
            else fprintf(stderr, "read: unexpected EOF after %llu of %llu bytes\n",
                         (unsigned long long)done,
                         (unsigned long long)expected);
            return false;
        }
        done += got;
    }

    int extra = fgetc(f);
    if (extra != EOF || ferror(f)) {
        fprintf(stderr, "read: input size changed while it was being loaded\n");
        return false;
    }
    return streamed_file_verify(source, "while it was being loaded");
}

typedef struct {
    const char *name;
    uint64_t    begin;
    uint64_t    end;
    bool        active;
} boot_range_t;

static bool add_u64(uint64_t a, uint64_t b, uint64_t *out) {
    if (!out || b > UINT64_MAX - a) return false;
    *out = a + b;
    return true;
}

static bool align_u64(uint64_t value, uint64_t alignment, uint64_t *out) {
    if (!alignment || (alignment & (alignment - 1)) ||
        value > UINT64_MAX - (alignment - 1)) return false;
    *out = (value + alignment - 1) & ~(alignment - 1);
    return true;
}

static bool boot_range_make(boot_range_t *range, const char *name,
                            uint64_t begin, uint64_t length, bool active) {
    if (!range || !name || !add_u64(begin, length, &range->end)) return false;
    range->name = name;
    range->begin = begin;
    range->active = active;
    return true;
}

static bool boot_ranges_overlap(const boot_range_t *a,
                                const boot_range_t *b) {
    if (!a->active || !b->active || a->begin == a->end || b->begin == b->end)
        return false;
    return a->begin < b->end && b->begin < a->end;
}

static bool boot_layout_validate(const boot_range_t *dram,
                                 const boot_range_t *ranges, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (!ranges[i].active) continue;
        if (ranges[i].begin < dram->begin || ranges[i].end > dram->end) {
            fprintf(stderr,
                    "layout: %s [0x%llx,0x%llx) is outside DRAM "
                    "[0x%llx,0x%llx)\n",
                    ranges[i].name,
                    (unsigned long long)ranges[i].begin,
                    (unsigned long long)ranges[i].end,
                    (unsigned long long)dram->begin,
                    (unsigned long long)dram->end);
            return false;
        }
        for (size_t j = 0; j < i; j++) {
            if (!boot_ranges_overlap(&ranges[i], &ranges[j])) continue;
            fprintf(stderr,
                    "layout: %s [0x%llx,0x%llx) overlaps %s "
                    "[0x%llx,0x%llx)\n",
                    ranges[i].name,
                    (unsigned long long)ranges[i].begin,
                    (unsigned long long)ranges[i].end,
                    ranges[j].name,
                    (unsigned long long)ranges[j].begin,
                    (unsigned long long)ranges[j].end);
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * The guest's /private/etc/fstab describes storage this machine does not have.
 *
 * Stock iPhone OS 3.1.3 ships:
 *
 *     /dev/disk0s1 / hfs ro 0 1
 *     /dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2
 *
 * disk0 on real hardware is published by AppleNANDFTL (raw NAND -> a linear
 * logical space) with IOFlashPartitionScheme on top of it cutting that space
 * into disk0s1/disk0s2. BOTH of those read undocumented Apple on-media
 * structures — IOFlashPartitionScheme logs "magic on partition table ...
 * doesn't match expected value" and "major version on partition table ...
 * does not match driver", i.e. it validates a format nobody outside Apple has
 * a specification for. Per the project rule we do not invent that layout, so
 * this VM has no disk0 at all: the system volume is the RAM disk md0
 * (bsd/dev/memdev.c), attached from the /chosen/memory-map RAMDisk property.
 *
 * (Worth recording for whoever revisits this: the undocumented part is the
 * NAND side, NOT partitioning in general. IOStorageFamily in this kernelcache
 * carries built-in IOGUIDPartitionScheme and IOFDiskPartitionScheme
 * personalities — both provider IOMedia, IOPropertyMatch Whole — so a block
 * device that ever does get published can be cut up with an ordinary,
 * documented GPT or MBR. What is missing is anything that would publish that
 * IOMedia *at boot*: there is no IOUSBMassStorageClass and no SCSI stack here,
 * and the kernel-side DiskImages entry point di_root_image() is called solely
 * by imageboot/netboot, neither of which is compiled into this kernelcache.
 * The DiskImages stack is not dead — /usr/libexec/mobile_image_mounter and
 * /usr/libexec/debug_image_mount both drive IOHDIXController directly — but
 * they are lockdownd services that attach a .dimage on host request, onto
 * /Developer, long after fstab has been consulted. Nothing attaches anything
 * during the launchd bootstrap, which is where /private/var is needed.)
 *
 * That matters because launchd's bootstrap (launchctl.c, launchd-321) treats
 * the boot-volume fsck as FATAL and everything after it as advisory:
 *
 *     statfs("/", &sfs);
 *     if (sfs.f_flags & MNT_RDONLY) {            // xnu always mounts / ro
 *         fwexec(fsck -p) || fwexec(fsck -fy)
 *             || (fputs("fsck failed!"), reboot(RB_HALT));   // <-- fatal
 *         path_check("/etc/fstab") ? fwexec(mount -vat nonfs)
 *                                  : fwexec(mount -uw /);    // <-- advisory
 *     }
 *
 * /sbin/fsck is the BSD wrapper: it reads /etc/fstab and fscks every entry
 * with a nonzero pass number. Pointed at a /dev/disk0s1 that cannot exist it
 * exits nonzero, twice, and launchd halts the machine. That is the whole
 * reason the boot ended in _halt_all_cpus.
 *
 * So the VM rewrites the record to name the device it actually provides. This
 * is a guest CONFIGURATION change — the same edit you would make porting an
 * OS image to different storage — not a guess at an Apple binary format.
 *
 * It is done as a byte-for-byte in-place overwrite of the exact stock string,
 * which must appear EXACTLY ONCE in the image, so no HFS+ catalog surgery is
 * needed and the file's logicalSize is unchanged; the replacement is padded to
 * the same length with an fstab comment line. If the stock bytes are missing
 * or ambiguous we refuse and say so rather than patch something else.
 * ------------------------------------------------------------------------- */
static const char FSTAB_STOCK[] =
    "/dev/disk0s1 / hfs ro 0 1\n"
    "/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2\n";

/* Returns the image offset patched, or (size_t)-1 on refusal. */
static size_t rd_rewrite_fstab(uint8_t *rd, size_t rd_n, const char *line) {
    const size_t stock_n = sizeof FSTAB_STOCK - 1;   /* 76 */
    size_t hit = (size_t)-1, hits = 0;

    for (size_t i = 0; i + stock_n <= rd_n; i++) {
        if (rd[i] == '/' && !memcmp(rd + i, FSTAB_STOCK, stock_n)) {
            if (!hits) hit = i;
            hits++;
        }
    }
    if (hits != 1) {
        fprintf(stderr,
                "fstab: stock record found %u times in the RAM disk, expected "
                "exactly 1 — refusing to patch.\n"
                "       Pass --keep-fstab if this image is deliberately not the "
                "stock 3.1.3 rootfs\n"
                "       (a restore ramdisk, say); silently booting on unchecked "
                "assumptions is\n"
                "       how you lose an afternoon to launchd halting the "
                "machine.\n", (unsigned)hits);
        return (size_t)-1;
    }

    size_t line_n = strlen(line);
    if (line_n + 1 > stock_n) {
        fprintf(stderr, "fstab: replacement is %u bytes, only %u fit in the "
                        "record\n", (unsigned)line_n + 1, (unsigned)stock_n);
        return (size_t)-1;
    }

    /* line "\n", then a comment line of exactly the remaining width. getfsent
     * ignores '#' lines, so the padding is inert but still valid fstab. */
    uint8_t *p = rd + hit;
    memcpy(p, line, line_n);
    p[line_n] = '\n';
    size_t pad = stock_n - line_n - 1;
    if (pad) {
        memset(p + line_n + 1, '#', pad - 1);
        p[stock_n - 1] = '\n';
    }
    return hit;
}

/* ---------------------------------------------------------------------------
 * The system volume has ZERO free blocks, and on this machine that is fatal to
 * everything launchd tries to start.
 *
 * MEASURED on firmware/rootfs.img (iPhone1,2 3.1.3 7E18, 018-6482-014.dmg):
 *
 *     signature HX  version 5  blockSize 4096
 *     totalBlocks 105780  (x 4096 == 433274880 == the image, exactly)
 *     freeBlocks  0
 *     allocation file: 16384 bytes, 1 extent at block 1, 4 blocks
 *     bits 0..105779 all set, bits 105780..131071 all clear
 *
 * That is not damage, it is Apple's build process. The system dmg is sized to
 * fit its contents because on hardware "/" is disk0s1 and stays read-only
 * forever; everything writable lives on disk0s2, a separate volume the restore
 * newfs'es. We have no disk0 at all (see the fstab note above), so "/" is the
 * only volume, we mount it rw — and there is not one free block on it. launchd
 * can create nothing: no /private/var/log/asl, no /private/var/db, no
 * lockdown records, no SpringBoard caches.
 *
 * The volume is therefore grown to make room. HFS+ is a documented format
 * (Apple TN1150), the image is not encrypted or compressed, and every one of
 * the four edits below is named in the spec, so this is not a guess at an
 * Apple layout — it is the layout, applied:
 *
 *   1. volumeHeader.totalBlocks  += n           (the volume gets bigger)
 *   2. volumeHeader.freeBlocks    = recount     (from the bitmap, not arithmetic)
 *   3. allocation file: free the block(s) covering the OLD reserved tail, and
 *      allocate the block(s) covering the NEW one
 *   4. write the alternate volume header at totalBlocks*blockSize - 1024
 *
 * Point 3 is the one worth spelling out. TN1150 puts the alternate volume
 * header 1024 bytes before the end of the volume and reserves the final 512
 * bytes; the allocation block(s) containing that tail are marked in-use and
 * belong to no file. (Verified independently: walking the catalog and the
 * extents overflow file over this image accounts for 105778 of the 105780
 * in-use blocks — the two left over are block 0, which holds the boot blocks
 * plus the primary header, and block 105779, which holds the alternate. Nor
 * COULD a file own that block: extents are whole allocation blocks, so a file
 * owning it would own the alternate header's bytes and be scribbled on every
 * time the volume was flushed.) So when the tail moves, the block it used to
 * occupy becomes ordinary free space and the block it moves to becomes
 * reserved.
 *
 * Getting that wrong in either direction is exactly what fsck_hfs exists to
 * catch, and launchd's bootstrap runs fsck_hfs on every boot here — but do not
 * lean on it. `fsck_hfs -p` QUICK-EXITS on a volume carrying
 * kHFSVolumeUnmounted, and forcing the real scan (by clearing that bit) stops
 * the machine on `SMULBB r6, r3, r5` at fsck_hfs+0x12130, which arm_interp.c
 * traps along with the rest of the SMULxy/SMLAxy DSP space. So the checking
 * here has to stand on its own: hfs_validate() runs over the bytes before the
 * edit AND again after it, and refuses rather than warns.
 *
 * The 16 KB allocation file is the ceiling: 16384 * 8 == 131072 bits, i.e.
 * 512 MB at this block size. Growing past that would mean allocating more
 * blocks TO the allocation file and rewriting its fork extents, which is a
 * different and much more invasive operation. We stop at the ceiling and say
 * so. (TN1150 explicitly allows an allocation file larger than the volume
 * needs, requiring only that the surplus bits be zero — which is why the
 * headroom is there in the first place: newfs_hfs rounded 105780 bits up to a
 * whole 4-block clump.)
 *
 * As with the fstab rewrite, only the in-memory copy is touched, and the RAM
 * disk published to the guest is the grown one, so the device the guest sees
 * and the volume on it are the same size — which fsck_hfs checks (it locates
 * the alternate header from the DEVICE size, via DKIOCGETBLOCKCOUNT).
 * ------------------------------------------------------------------------- */
static uint16_t hbe16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t hbe32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}
static uint64_t hbe64(const uint8_t *p) {
    return ((uint64_t)hbe32(p) << 32) | hbe32(p + 4);
}
static void hput32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

#define HFS_VH_OFF      1024u     /* TN1150: volume header at byte 1024      */
#define HFS_VH_LEN      512u      /* ...and it is 512 bytes long             */
#define HFS_SIG_HFSPLUS 0x482Bu   /* 'H+' */
#define HFS_SIG_HFSX    0x4858u   /* 'HX' */
#define HFS_ATTR_JOURNALED (1u << 13)

typedef struct {
    uint32_t block_size, total_blocks, free_blocks, next_alloc, attributes;
    uint64_t alloc_bytes;             /* allocation file logicalSize          */
    uint32_t alloc_fork_blocks;
    uint32_t ext_start[8], ext_count[8];
    uint32_t nbits;                   /* bits the allocation file can address */
} hfsvol_t;

/* Image offset of byte `off` of the allocation file, or (size_t)-1 if that
 * byte is past the fork's extents. */
static size_t hfs_alloc_off(const hfsvol_t *v, uint64_t off) {
    uint64_t seen = 0;
    for (unsigned i = 0; i < 8; i++) {
        uint64_t span = (uint64_t)v->ext_count[i] * v->block_size;
        if (!span) continue;
        if (off < seen + span)
            return (size_t)((uint64_t)v->ext_start[i] * v->block_size + (off - seen));
        seen += span;
    }
    return (size_t)-1;
}

static int hfs_bit_get(const uint8_t *img, const hfsvol_t *v, uint32_t bit) {
    size_t o = hfs_alloc_off(v, bit >> 3);
    return (img[o] >> (7 - (bit & 7))) & 1;
}
static void hfs_bit_set(uint8_t *img, const hfsvol_t *v, uint32_t bit, int on) {
    size_t o = hfs_alloc_off(v, bit >> 3);
    uint8_t m = (uint8_t)(1u << (7 - (bit & 7)));
    if (on) img[o] |= m; else img[o] &= (uint8_t)~m;
}

/*
 * Parse and FULLY validate the volume. Every field this tool is about to touch
 * or rely on is checked, and anything unexpected is a refusal, not a warning:
 * a half-understood volume that boots is worse than one that does not, because
 * the damage shows up later as an unexplained guest failure.
 *
 * Run before the edit and again after it, so the "after" pass is an
 * independent re-derivation from the bytes rather than a restatement of what
 * the edit intended.
 */
static bool hfs_validate(const uint8_t *img, size_t n, hfsvol_t *v,
                         const char *when) {
#define VBAD(...) do { fprintf(stderr, "rd-grow (%s): ", when); \
                       fprintf(stderr, __VA_ARGS__); return false; } while (0)
    memset(v, 0, sizeof *v);
    if (n < HFS_VH_OFF + HFS_VH_LEN)
        VBAD("image is %llu bytes — too small for a volume header\n",
             (unsigned long long)n);

    const uint8_t *vh = img + HFS_VH_OFF;
    uint16_t sig = hbe16(vh), ver = hbe16(vh + 2);
    if (!((sig == HFS_SIG_HFSPLUS && ver == 4) || (sig == HFS_SIG_HFSX && ver == 5)))
        VBAD("signature '%c%c' version %u is neither HFS+ (H+/4) nor HFSX "
             "(HX/5) — not growing something this tool does not understand\n",
             vh[0] >= 32 && vh[0] < 127 ? vh[0] : '?',
             vh[1] >= 32 && vh[1] < 127 ? vh[1] : '?', ver);

    v->attributes   = hbe32(vh + 4);
    v->block_size   = hbe32(vh + 40);
    v->total_blocks = hbe32(vh + 44);
    v->free_blocks  = hbe32(vh + 48);
    v->next_alloc   = hbe32(vh + 52);
    uint32_t journal_info_block = hbe32(vh + 12);

    if (v->block_size < 512 || v->block_size > (1u << 20) ||
        (v->block_size & (v->block_size - 1)) || (v->block_size % 512))
        VBAD("blockSize %u is not a power-of-two multiple of 512\n", v->block_size);
    if (!v->total_blocks) VBAD("totalBlocks is 0\n");
    if ((uint64_t)v->total_blocks * v->block_size != (uint64_t)n)
        VBAD("totalBlocks %u x blockSize %u = %llu but the image is %llu bytes "
             "— the volume does not fill the image, and this tool only handles "
             "a bare volume with no partition map around it\n",
             v->total_blocks, v->block_size,
             (unsigned long long)v->total_blocks * v->block_size,
             (unsigned long long)n);
    if (v->free_blocks > v->total_blocks)
        VBAD("freeBlocks %u exceeds totalBlocks %u\n", v->free_blocks, v->total_blocks);
    if (v->next_alloc >= v->total_blocks)
        VBAD("nextAllocation %u is outside the volume (%u blocks)\n",
             v->next_alloc, v->total_blocks);
    /* A journalled volume has a transaction log that would also have to be
     * reasoned about; this image is not journalled and we do not pretend to
     * handle one that is. */
    if ((v->attributes & HFS_ATTR_JOURNALED) || journal_info_block)
        VBAD("volume is journalled (attributes 0x%08x, journalInfoBlock %u) — "
             "not supported\n", v->attributes, journal_info_block);

    /* The alternate volume header, 1024 bytes before the end (TN1150). */
    const uint8_t *avh = img + n - HFS_VH_OFF;
    if (memcmp(vh, avh, HFS_VH_LEN))
        VBAD("the alternate volume header at 0x%llx does not match the primary "
             "— refusing to edit a volume whose two headers already disagree\n",
             (unsigned long long)(n - HFS_VH_OFF));

    /* Allocation file fork descriptor, at offset 112 of the volume header. */
    const uint8_t *af = vh + 112;
    v->alloc_bytes      = hbe64(af);
    v->alloc_fork_blocks = hbe32(af + 12);
    uint32_t summed = 0;
    for (unsigned i = 0; i < 8; i++) {
        v->ext_start[i] = hbe32(af + 16 + i * 8);
        v->ext_count[i] = hbe32(af + 20 + i * 8);
        if (!v->ext_count[i]) continue;
        if ((uint64_t)v->ext_start[i] + v->ext_count[i] > v->total_blocks)
            VBAD("allocation file extent %u (%u+%u) runs past the volume\n",
                 i, v->ext_start[i], v->ext_count[i]);
        summed += v->ext_count[i];
    }
    if (summed != v->alloc_fork_blocks)
        VBAD("allocation file has %u blocks but its 8 inline extents cover %u "
             "— it spills into the extents overflow file, which this tool does "
             "not read\n", v->alloc_fork_blocks, summed);
    if (v->alloc_bytes > (uint64_t)v->alloc_fork_blocks * v->block_size)
        VBAD("allocation file logicalSize %llu exceeds its %u allocated blocks\n",
             (unsigned long long)v->alloc_bytes, v->alloc_fork_blocks);
    if (v->alloc_bytes > 0xffffffffull / 8)
        VBAD("allocation file is implausibly large (%llu bytes)\n",
             (unsigned long long)v->alloc_bytes);
    v->nbits = (uint32_t)(v->alloc_bytes * 8);
    if (v->nbits < v->total_blocks)
        VBAD("allocation file addresses %u blocks, volume has %u\n",
             v->nbits, v->total_blocks);

    /* The bitmap must agree with freeBlocks, and every bit past the end of the
     * volume must be zero (TN1150 requires the surplus to be clear). Both are
     * cheap and both are exactly what a bad edit would break. */
    uint32_t used = 0;
    for (uint32_t b = 0; b < v->total_blocks; b++) used += (uint32_t)hfs_bit_get(img, v, b);
    if (used != v->total_blocks - v->free_blocks)
        VBAD("the bitmap marks %u blocks in use but freeBlocks %u says %u\n",
             used, v->free_blocks, v->total_blocks - v->free_blocks);
    for (uint32_t b = v->total_blocks; b < v->nbits; b++)
        if (hfs_bit_get(img, v, b))
            VBAD("bit %u is set but lies past the end of the volume (%u blocks)"
                 " — TN1150 requires the surplus bits to be zero\n",
                 b, v->total_blocks);
    return true;
#undef VBAD
}

/* First and last allocation block covering the volume's reserved tail: the
 * alternate volume header (last 1024 bytes) and the reserved 512 after it. */
static uint32_t hfs_tail_first(uint32_t total_blocks, uint32_t block_size) {
    return (uint32_t)(((uint64_t)total_blocks * block_size - HFS_VH_OFF) / block_size);
}

/*
 * Grow the volume in a preallocated span by `want_mb` megabytes of FREE space.
 * The caller supplies capacity, so this never reallocates the hundreds-of-MiB
 * image or creates a second full-size host copy. On success rd_np is updated to
 * the exposed length. Returns false, with an explanation, on any refusal — the
 * caller aborts the boot rather than booting an image it did not understand.
 */
static bool rd_grow_volume(uint8_t *img, size_t *rd_np, size_t capacity,
                           uint32_t want_mb) {
    if (!img || !rd_np || *rd_np > capacity) {
        fprintf(stderr, "rd-grow: invalid destination span\n");
        return false;
    }
    size_t n = *rd_np;
    hfsvol_t v;

    if (!hfs_validate(img, n, &v, "before")) return false;

    uint32_t bs = v.block_size;
    uint64_t want_blocks = ((uint64_t)want_mb << 20) / bs;
    /* The tail block we hand back when the reserved region moves is one free
     * block we get for nothing, so ask for one fewer. */
    if (want_blocks) want_blocks--;

    uint64_t want_total = (uint64_t)v.total_blocks + want_blocks;
    uint32_t new_total  = want_total > v.nbits ? v.nbits : (uint32_t)want_total;
    if (new_total <= v.total_blocks) {
        fprintf(stderr, "rd-grow: %u MB is less than one allocation block "
                        "(%u bytes) of growth\n", want_mb, bs);
        return false;
    }
    if (want_total > v.nbits)
        printf("rd-grow    : clamped to the allocation file's %u-bit ceiling "
               "(%llu MB volume);\n"
               "             growing past it means enlarging the allocation "
               "file itself.\n",
               v.nbits, (unsigned long long)((uint64_t)v.nbits * bs) >> 20);

    uint32_t old_tail = hfs_tail_first(v.total_blocks, bs);
    uint32_t new_tail = hfs_tail_first(new_total, bs);

    /* The old reserved tail must be exactly as TN1150 describes it before we
     * hand those blocks back. If it is not, something else owns them and
     * freeing them would corrupt a file. */
    for (uint32_t b = old_tail; b < v.total_blocks; b++)
        if (!hfs_bit_get(img, &v, b)) {
            fprintf(stderr, "rd-grow: block %u holds the alternate volume "
                    "header but is marked FREE — refusing to touch this "
                    "volume\n", b);
            return false;
        }
    for (uint32_t b = new_tail; b < new_total; b++)
        if (b >= v.total_blocks && hfs_bit_get(img, &v, b)) {
            fprintf(stderr, "rd-grow: block %u is past the volume yet already "
                    "allocated\n", b);
            return false;
        }

    uint64_t new_n64 = (uint64_t)new_total * bs;
    if (new_n64 > SIZE_MAX || new_n64 > capacity) {
        fprintf(stderr,
                "rd-grow: %llu-byte result exceeds the %llu-byte destination "
                "capacity\n",
                (unsigned long long)new_n64,
                (unsigned long long)capacity);
        return false;
    }
    size_t new_n = (size_t)new_n64;
    memset(img + n, 0, new_n - n);

    /* (3) the reserved tail moves. */
    for (uint32_t b = old_tail; b < v.total_blocks; b++) hfs_bit_set(img, &v, b, 0);
    for (uint32_t b = new_tail; b < new_total;      b++) hfs_bit_set(img, &v, b, 1);

    /* (2) freeBlocks is RECOUNTED from the bitmap, never computed from the
     * growth, so the header cannot end up describing a bitmap we did not
     * actually write. */
    uint32_t used = 0;
    for (uint32_t b = 0; b < new_total; b++) used += (uint32_t)hfs_bit_get(img, &v, b);

    uint8_t *vh = img + HFS_VH_OFF;
    hput32(vh + 44, new_total);              /* (1) totalBlocks               */
    hput32(vh + 48, new_total - used);       /* (2) freeBlocks                */
    hput32(vh + 52, old_tail);               /* nextAllocation: first free    */

    /* (4) the alternate header, 1024 bytes before the new end. */
    memcpy(img + new_n - HFS_VH_OFF, vh, HFS_VH_LEN);

    *rd_np = new_n;

    hfsvol_t after;
    if (!hfs_validate(img, new_n, &after, "after")) return false;
    if (after.total_blocks != new_total || after.free_blocks != new_total - used) {
        fprintf(stderr, "rd-grow (after): header re-read as %u/%u blocks, "
                "expected %u/%u\n", after.total_blocks, after.free_blocks,
                new_total, new_total - used);
        return false;
    }

    printf("rd-grow    : volume %u -> %u blocks of %u  (%llu -> %llu MB), "
           "free %u -> %u blocks (%llu MB)\n"
             /* NOT "fsck_hfs will check this": it quick-exits on a volume
              * carrying kHFSVolumeUnmounted, and forcing the full scan stops
              * the machine on an unimplemented SMULBB inside fsck_hfs. The
              * guarantee here is hfs_validate() before and after, plus the
              * kernel's own mount. See docs/BOOTLOG.md. */
           "             alternate volume header rewritten at image+0x%llx; "
           "revalidated from the bytes after writing.\n",
           v.total_blocks, new_total, bs,
           (unsigned long long)((uint64_t)v.total_blocks * bs) >> 20,
           (unsigned long long)((uint64_t)new_total * bs) >> 20,
           v.free_blocks, new_total - used,
           (unsigned long long)((uint64_t)(new_total - used) * bs) >> 20,
           (unsigned long long)(new_n - HFS_VH_OFF));
    return true;
}

/* ---------------------------------------------------------------------------
 * Address naming.
 *
 * Both sources — the kernel's own LC_SYMTAB and the prelinked kext load map in
 * __PRELINK_INFO — live in core/src/firmware/ksyms.c so that machoinfo can
 * print the same map without booting anything. Everything here is the thin
 * layer above it: a rotating buffer so several names can appear in one printf,
 * and the physical/virtual normalisation, because before the MMU comes on the
 * guest executes at the physical alias of every symbol.
 *
 * The point of the kext half: a hot PC at 0xc0778123 used to be reported as an
 * unsymbolized address and cost a whole diagnosis cycle to attribute. It is
 * now "com.apple.driver.AppleMBX+0x122" the first time it is printed.
 * ------------------------------------------------------------------------- */
#define DTNAME 32                 /* device-tree property names are char[32] */

static ksyms_t  KS;
static uint32_t g_virt_base, g_phys_base;

static uint32_t ld32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Kernel virtual form of a PC we caught. Idempotent: an address that is
 * already virtual is above virt_base and passes straight through, so call
 * sites that convert by hand are harmless. */
static uint32_t to_va(uint32_t a) {
    if (g_virt_base && a >= g_phys_base && a < g_virt_base)
        return a - g_phys_base + g_virt_base;
    return a;
}

static const char *ksym_at(uint32_t addr) {
    static char buf[4][192];
    static unsigned turn;
    char *out = buf[turn++ & 3];
    return ksyms_resolve(&KS, to_va(addr), out, sizeof buf[0]);
}

static uint32_t ksym_value(const char *name) { return ksyms_value(&KS, name); }

/* The load map, printed by -L and by machoinfo -k. */
static void dump_kext_map(void) {
    printf("=== PRELINKED KEXT LOAD MAP (__PRELINK_TEXT 0x%08x..0x%08x) ===\n",
           KS.prelink_lo, KS.prelink_hi);
    printf("    %-44s %-10s %-10s %-9s %s\n",
           "bundle identifier", "load addr", "end", "size", "kmod_info");
    for (unsigned i = 0; i < KS.nkext; i++) {
        const kext_t *k = &KS.kext[i];
        if (!k->has_exec) continue;
        printf("    %-44s 0x%08x 0x%08x %-9u 0x%08x\n",
               k->bundle, k->addr, k->addr + k->size, k->size, k->kmod_info);
    }
    printf("    --- no executable (KPI pseudo-extensions, never a hot PC) ---\n");
    for (unsigned i = 0; i < KS.nkext; i++)
        if (!KS.kext[i].has_exec) printf("    %s\n", KS.kext[i].bundle);
}

/*
 * Say out loud what we can and cannot name. A profile that reports a bare
 * address is the failure mode this whole module exists to remove, so if the
 * kext map did not parse, the boot log has to shout about it rather than
 * quietly go back to printing hex.
 */
static void report_symbol_sources(void) {
    printf("symbols    : %u kernel symbols (%s)\n",
           KS.nsym, ksyms_strerror(KS.sym_status));
    if (KS.prelink_status == KSYMS_OK) {
        printf("kexts      : %u prelinked, %u with code, 0x%08x..0x%08x\n",
               KS.nkext, KS.nkext_exec, KS.prelink_lo, KS.prelink_hi);
        if (KS.detail[0]) printf("  ! %s\n", KS.detail);
    } else {
        printf("\n"
               "  ############################################################\n"
               "  # KEXT LOAD MAP UNAVAILABLE — hot PCs inside a prelinked   #\n"
               "  # driver will print as bare addresses again.               #\n"
               "  #   %s\n"
               "  #   %s\n"
               "  ############################################################\n\n",
               ksyms_strerror(KS.prelink_status),
               KS.detail[0] ? KS.detail : "(no further detail)");
    }
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
 * Break a node's "compatible" so no driver matches it. IOKit matches AppleMBX
 * (and friends) purely by IONameMatch against this string; flipping the first
 * byte leaves the length unchanged (so the flat tree is not disturbed) and
 * makes the name match nothing. Used to keep the MBX/PowerVR graphics driver
 * from starting: it busy-polls a wrapper-reset bit in its 16 MB register block
 * that we do not model, which wedges the whole boot at ~72% of runtime. iPhone
 * OS 3 has a software-blit path (LK_ENABLE_MBX2D=0), so the GPU is not required
 * to reach a rendered SpringBoard.
 */
static bool dt_unmatch(uint8_t *b, size_t len, const char *path) {
    size_t node = dtn_path(b, len, path);
    if (node == DT_NONE) { printf("  dt: node /%s NOT FOUND\n", path); return false; }
    uint32_t vl = 0;
    uint8_t *p = dtn_prop(b, len, node, "compatible", &vl);
    if (!p || !vl) { printf("  dt: /%s:compatible missing\n", path); return false; }
    char was = (char)p[0];
    p[0] = 'x';                      /* 'm'bx... -> 'x'bx..., matches nothing */
    printf("  dt: /%-14s compatible '%c...' -> 'x...' (unmatched)\n", path, was);
    return true;
}

/* ---------------------------------------------------------------------------
 * Adding an entry to /chosen/memory-map WITHOUT rebuilding the blob.
 *
 * The obvious problem: this flat format has no relocation table, every offset
 * is implicit in the byte stream, so appending a property to a node in the
 * middle of the tree means rewriting everything after it. That is why every
 * other patch here is same-length and in place.
 *
 * It turns out no rebuild is needed. The shipped tree already carries sixteen
 * placeholders in that node — MemoryMapReserved-0 .. MemoryMapReserved-15 —
 * each with a value of exactly 8 zero bytes, which is exactly the shape of a
 * memory-map entry ({address, size}). That is not a coincidence: it is how
 * iBoot adds entries at run time. It finds a spare placeholder, overwrites the
 * 32-byte name field with the real key, and fills in the two words. Name and
 * value lengths are both unchanged, so the patch is same-length and in place.
 *
 * The one live entry in the shipped tree corroborates the format:
 *   .DeviceTree = {0x00000000, 0x00009e60}   and 0x9e60 == 40544 == the exact
 * size of devicetree.bin. Address zero because iBoot had not run yet.
 *
 * So we do what iBoot does. `claim` is CONFIRMED behaviour of the format, not
 * a trick: renaming a reserved slot is the mechanism the slots exist for.
 * ------------------------------------------------------------------------- */
static bool dt_memmap_add(uint8_t *b, size_t len, const char *key,
                          uint32_t addr, uint32_t size) {
    size_t node = dtn_path(b, len, "chosen/memory-map");
    if (node == DT_NONE) {
        printf("  dt: /chosen/memory-map NOT FOUND — cannot add %s\n", key);
        return false;
    }
    uint32_t np, nc;
    size_t p = dtn_hdr(b, len, node, &np, &nc);
    if (!p) return false;

    size_t slot = 0;                       /* offset of the property record */
    for (uint32_t i = 0; i < np; i++) {
        if (p + 36 > len) return false;
        char nm[DTNAME + 1];
        memcpy(nm, b + p, DTNAME); nm[DTNAME] = '\0';
        uint32_t l = ld32(b + p + 32) & 0x7fffffffu;
        /* An existing entry with this key wins: re-running must be idempotent
         * rather than burning a second placeholder each time. */
        if (!strcmp(nm, key) && l == 8) { slot = p; break; }
        if (!slot && l == 8 && !strncmp(nm, "MemoryMapReserved-", 18) &&
            ld32(b + p + 36) == 0 && ld32(b + p + 40) == 0)
            slot = p;                      /* first free placeholder; keep looking
                                            * in case the real key already exists */
        p += 36 + ((l + 3u) & ~3u);
        if (p > len) return false;
    }
    if (!slot) {
        printf("  dt: no free MemoryMapReserved-* placeholder for %s\n", key);
        return false;
    }
    char old[DTNAME + 1];
    memcpy(old, b + slot, DTNAME); old[DTNAME] = '\0';
    memset(b + slot, 0, DTNAME);
    memcpy(b + slot, key, strlen(key) < DTNAME ? strlen(key) : DTNAME - 1);
    uint8_t *v = b + slot + 36;
    uint32_t vals[2] = { addr, size };
    for (int i = 0; i < 2; i++) {
        v[i*4+0] = (uint8_t)vals[i];         v[i*4+1] = (uint8_t)(vals[i] >> 8);
        v[i*4+2] = (uint8_t)(vals[i] >> 16); v[i*4+3] = (uint8_t)(vals[i] >> 24);
    }
    printf("  dt: /chosen/memory-map  %-18s -> %-10s {0x%08x,0x%08x}  (in place)\n",
           old, key, addr, size);
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

/* iBoot reads the three Syrah panel-identification bytes as a big-endian
 * 24-bit value (openiBoot, plat-s5l8900/lcd.c:syrah_init). Its N82/iPhone 3G
 * hardware log records a5:c2:2b. In this repo's shipped AppleMerlotLCD binary,
 * 0xc0651f60 rejects ID zero and 0x8000 and 0xc0651770 branches on the ID's low
 * three bits. The zero in the IPSW tree is therefore only a placeholder. */
#define N82_LCD_PANEL_ID 0x00a5c22bu
#define N82_FB_WIDTH     320u
#define N82_FB_HEIGHT    480u
#define N82_FB_BPP       4u
#define N82_FB_BYTES     (N82_FB_WIDTH * N82_FB_HEIGHT * N82_FB_BPP)

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

static const uint8_t *guest_ptr(uint32_t va, uint32_t need) {
    uint32_t pa = va - g_virt_base + g_phys_base;
    if (va < g_virt_base) pa = va;                     /* already physical */
    if (pa < g_mach->ram_base) return NULL;
    uint32_t o = pa - g_mach->ram_base;
    if (o >= g_mach->ram_size || g_mach->ram_size - o < need) return NULL;
    return g_mach->ram + o;
}

/* Fetch bytes through the guest's current translation tables. guest_ptr()
 * intentionally handles only the kernel's fixed linear window, but the most
 * valuable undefined instructions now occur in user processes. Re-walk the
 * exact fetch mapping after a stop so the report always includes the encoding
 * that blocked progress. The stopped instruction already fetched once, so
 * this diagnostic read cannot introduce a mapping the guest did not use. */
static bool guest_fetch_bytes(uint32_t va, uint8_t *out, size_t n,
                              uint32_t *first_pa) {
    if (!g_mach || !out || n > UINT32_MAX ||
        (n && (uint64_t)va + n - 1u > UINT32_MAX)) return false;

    uint32_t mode = g_mach->cpu.cpsr & ARM_CPSR_MODE_MASK;
    bool priv = mode != ARM_MODE_USR;
    for (size_t i = 0; i < n; i++) {
        uint32_t pa = 0;
        uint32_t fsr = arm_mmu_translate(&g_mach->cpu, va + (uint32_t)i,
                                         ARM_ACCESS_FETCH, priv, &pa);
        if (fsr || pa < g_mach->ram_base) return false;
        uint64_t off = (uint64_t)pa - g_mach->ram_base;
        if (off >= g_mach->ram_size) return false;
        if (i == 0 && first_pa) *first_pa = pa;
        out[i] = g_mach->ram[(size_t)off];
    }
    return true;
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

/* ---------------------------------------------------------------------------
 * The guest's OWN view of memory.
 *
 * The budget printed in the header is the host's arithmetic. This is the
 * kernel's answer to the same question, read straight out of its globals, and
 * the two disagreeing is itself information (it would mean the kernel is not
 * seeing the DRAM we think we gave it).
 *
 * vm_page_free_count is a PAGE count, sampled rather than watched: the number
 * that matters for "does a 92 MB shared cache fit" is not the value at the end
 * of the run but the LOW-WATER MARK across it, because a boot that survives on
 * average and runs out once still dies. Sampling is on the same phase as the
 * profiler, so it costs four guest reads per 64K instructions.
 *
 * All of these are plain kernel-window globals, so guest_ptr's linear
 * translation reaches them without an MMU walk. A symbol that does not resolve
 * reads as 0 and prints as "-", never as a plausible wrong number.
 * ------------------------------------------------------------------------- */
static uint32_t guest_u32(uint32_t va) {
    const uint8_t *p = va ? guest_ptr(va, 4) : NULL;
    return p ? ld32(p) : 0u;
}

static struct {
    uint32_t free_va, wire_va, active_va, inactive_va, target_va;
    uint32_t sane_va, maxmem_va, memsize_va;
    uint32_t availstart_va, availend_va, firstavail_va;
    uint32_t free_peak;         /* most free pages ever seen = end of bootstrap */
    unsigned free_peak_at;
    uint32_t free_min;          /* low-water mark AFTER the peak, in pages */
    unsigned free_min_at;       /* instruction index of that minimum */
    uint32_t free_first;        /* first non-zero sample */
    unsigned free_first_at;
    unsigned samples;
    uint32_t hist[40];          /* free pages over the run, 40 buckets */
    unsigned hist_n[40];
} VM;

static void vm_resolve(void) {
    VM.free_va       = ksym_value("_vm_page_free_count");
    VM.wire_va       = ksym_value("_vm_page_wire_count");
    VM.active_va     = ksym_value("_vm_page_active_count");
    VM.inactive_va   = ksym_value("_vm_page_inactive_count");
    VM.target_va     = ksym_value("_vm_page_free_target");
    VM.sane_va       = ksym_value("_sane_size");
    VM.maxmem_va     = ksym_value("_max_mem");
    VM.memsize_va    = ksym_value("_mem_size");
    VM.availstart_va = ksym_value("_avail_start");
    VM.availend_va   = ksym_value("_avail_end");
    VM.firstavail_va = ksym_value("_first_avail");
    VM.free_min = 0xffffffffu;
}

static void vm_sample(unsigned n, unsigned steps) {
    if (!VM.free_va) return;
    uint32_t f = guest_u32(VM.free_va);
    /* Zero means vm_page_bootstrap has not run yet, not "out of memory". */
    if (!f) return;
    VM.samples++;
    if (!VM.free_first) { VM.free_first = f; VM.free_first_at = n; }
    /*
     * The low-water mark is only meaningful AFTER the pool is full.
     * vm_page_bootstrap fills the free list one page at a time, so the first
     * samples catch it mid-fill and a naive minimum reports "0.38 MB free",
     * which reads as catastrophic memory pressure and is nothing of the kind.
     * Track the peak, and only start looking for a minimum once past it.
     */
    if (f >= VM.free_peak) { VM.free_peak = f; VM.free_peak_at = n; VM.free_min = f; }
    else if (f < VM.free_min) { VM.free_min = f; VM.free_min_at = n; }
    unsigned b = steps ? (unsigned)((uint64_t)n * 40u / steps) : 0;
    if (b > 39) b = 39;
    VM.hist[b] += f;
    VM.hist_n[b]++;
}

#define VM_MB(pages) ((double)(pages) * 4096.0 / 1048576.0)

static void vm_report(void) {
    printf("\n=== GUEST VM COUNTERS (the kernel's own view) ===\n");
    if (!VM.free_va) {
        printf("    _vm_page_free_count did not resolve — no report\n");
        return;
    }
    struct { const char *nm; uint32_t va; bool pages; } row[] = {
        { "mem_size",              VM.memsize_va,    false },
        { "max_mem",               VM.maxmem_va,     false },
        { "sane_size",             VM.sane_va,       false },
        { "avail_start",           VM.availstart_va, false },
        { "avail_end",             VM.availend_va,   false },
        { "first_avail",           VM.firstavail_va, false },
        { "vm_page_free_count",    VM.free_va,       true  },
        { "vm_page_free_target",   VM.target_va,     true  },
        { "vm_page_wire_count",    VM.wire_va,       true  },
        { "vm_page_active_count",  VM.active_va,     true  },
        { "vm_page_inactive_count",VM.inactive_va,   true  },
    };
    for (unsigned i = 0; i < sizeof row / sizeof row[0]; i++) {
        if (!row[i].va) { printf("    %-24s -\n", row[i].nm); continue; }
        uint32_t v = guest_u32(row[i].va);
        if (row[i].pages)
            printf("    %-24s %10u pages  %8.2f MB\n", row[i].nm, v, VM_MB(v));
        else
            printf("    %-24s 0x%08x       %8.2f MB\n", row[i].nm, v, v / 1048576.0);
    }
    if (VM.samples) {
        printf("    ---- free pages over the run (%u samples) ----\n", VM.samples);
        printf("    first  %8u pages %7.2f MB  @instr %u  (mid-bootstrap)\n",
               VM.free_first, VM_MB(VM.free_first), VM.free_first_at);
        printf("    peak   %8u pages %7.2f MB  @instr %u  (pool fully built)\n",
               VM.free_peak, VM_MB(VM.free_peak), VM.free_peak_at);
        printf("    LOWEST %8u pages %7.2f MB  @instr %u   <-- the headroom "
               "that actually has to hold everything\n",
               VM.free_min, VM_MB(VM.free_min), VM.free_min_at);
        for (unsigned i = 0; i < 40; i++)
            if (VM.hist_n[i])
                printf("      bucket %2u  mean free %8.2f MB\n",
                       i, VM_MB((double)VM.hist[i] / VM.hist_n[i]));
    } else {
        printf("    (never sampled non-zero — vm_page_bootstrap not reached)\n");
    }
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
    /* The M5 frontier: pid 1 exec'ing /sbin/launchd, and what it hits on the
     * way. grade_binary is the gate that used to answer 0 — CPU_SUBTYPE_ARM_ALL
     * against an ARMv6 Mach-O — and turn the exec into EBADARCH. The rest say
     * whether we then get past code signing (cs_validate_page / ubc_cs_blob_add)
     * and the MAC policy (mac_vnode_check_exec), and whether the new image ever
     * reaches user mode (thread_bootstrap_return) and issues a system call. */
    { "_load_init_program",         0xc011d9acu },
    { "_execve",                    0xc011d98cu },
    { "_load_machfile",             0xc014c508u },
    { "_grade_binary",              0xc0153f90u },
    { "_mac_vnode_check_exec",      0xc01acb38u },
    { "_ubc_cs_blob_add",           0xc013b2a4u },
    { "_cs_validate_page",          0xc013af54u },
    { "_thread_bootstrap_return",   0xc006871cu },
    { "_unix_syscall",              0xc01541a8u },
    { "_bsdinit_task",              0xc0110868u },
    /* Which exception the userspace image is actually taking. A first-level
     * handler count that climbs without _unix_syscall ever being reached means
     * pid 1 is spinning on a fault rather than making progress. */
    { "_fleh_swi",                  0xc00680a0u },
    { "_fleh_undef",                0xc0067ff8u },
    { "_fleh_prefabt",              0xc006828cu },
    { "_fleh_dataabt",              0xc0068338u },
    { "_sleh_undef",                0xc006c184u },
    { "_sleh_abort",                0xc006c538u },
    { "_vm_fault",                  0xc003e6f0u },
    { "_exception_triage",          0xc001c708u },
    { "_mach_msg_overwrite_trap",   0xc001899cu },
    { "_cs_invalid_page",           0xc0121e5eu },
    { "_psignal",                   0xc0127278u },
    { "_exit1",                     0xc011f63cu },
    /* Three PCs inside cs_validate_page (0xc013af54), which is the verdict on
     * one page of a signed executable. They separate the three ways it can say
     * no, and only one of them is our bug:
     *   no_hash_exit  0xc013b11a — no blob covers this page, or the code
     *                              directory has no hash slot for it: policy.
     *   hashing       0xc013b0ca — SHA1Init, i.e. it found a hash and is about
     *                              to compute the page's own.
     *   bad_hash      0xc013b0f8 — the bcmp failed. The page we handed the
     *                              kernel is not the page the signature was
     *                              made over, which is a data-path bug on our
     *                              side, not a signing policy we can opt out of. */
    { "cs_validate:no_hash_exit",   0xc013b11au },
    { "cs_validate:hashing",        0xc013b0cau },
    { "cs_validate:bad_hash",       0xc013b0f8u },
    { "_panic",                     0xc001c13cu },
    { "_Debugger",                  0xc006abbcu },
};
#define NMILE ((unsigned)(sizeof MILESTONES / sizeof MILESTONES[0]))

/*
 * Milestones resolved BY NAME out of the kernel's own symbol table instead of
 * being hand-transcribed. Everything here answers one question — "is the
 * machine still doing work, or has every thread parked?" — so the list is the
 * scheduler, the sleep primitives, the Mach IPC receive path, and the two
 * dyld-facing syscalls that a launchd which cannot map the shared cache would
 * be stuck at. A name that does not resolve is dropped silently at startup and
 * reported in the milestone table by its absence, never by a wrong address.
 */
static const char *MILE_BYNAME[] = {
    /* scheduler / idle: if these are the whole late window, nothing is runnable */
    "_machine_idle", "_idle_thread", "_thread_block_reason", "_thread_invoke",
    "_thread_continue", "_thread_go", "_swtch", "_thread_switch",
    "_wait_queue_assert_wait", "_clock_delay_until", "_delay",
    /* BSD sleep */
    "_msleep", "_tsleep", "_tsleep0", "_msleep0",
    /* Mach IPC receive — a launchd blocked on a port sits here */
    "_mach_msg_trap", "_ipc_mqueue_receive", "_ipc_mqueue_receive_continue",
    "_semaphore_wait_continue",
    /* the dyld shared cache path */
    "_shared_region_map_np", "_shared_region_check_np",
    /* VM / exec paths worth seeing counts for */
    "_mmap", "_vm_map_enter", "_kernel_thread_start",
};
#define NMILE_BYNAME ((unsigned)(sizeof MILE_BYNAME / sizeof MILE_BYNAME[0]))
#define NMILE_MAX (NMILE + NMILE_BYNAME)

/* The runtime table actually scanned by the step loop: the hand-written list
 * above followed by whichever of MILE_BYNAME the symbol table could resolve. */
static milestone_t MILE[NMILE_MAX];
static unsigned    NM;

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
    uint32_t    mile_pa[NMILE_MAX];
    uint64_t    mile_hits[NMILE_MAX];
    unsigned    mile_first[NMILE_MAX];

    /* --- WHAT MODE THE GUEST IS ACTUALLY IN ------------------------------
     * Indexed by CPSR[4:0]. The single most decisive number for a stall is the
     * USER-mode count inside a late window: zero user instructions after the
     * exec means pid 1 never runs, so it is blocked in the kernel and the
     * question is "on what", not "where in launchd".  */
    uint64_t    mode_all[32], mode_win[32];
    uint64_t    thumb_all, thumb_win;
    uint64_t    win_instrs;

    /* --- USER-MODE PC HISTOGRAM ------------------------------------------
     * Sampled on EVERY user-mode instruction, not every 1024: user mode is
     * rare enough here that a 1-in-1024 sample of it rounds to nothing. */
#define UPCHASH 4096u
    unsigned    upc_n, upc_dropped;
    struct { uint32_t va; uint64_t hits; } upc_hist[UPCHASH];

    /* --- SYSTEM CALLS ----------------------------------------------------
     * Captured at _fleh_swi, whose FIRST instruction is `cmn ip,#3` — so the
     * ABI is settled by the handler itself: r12 carries the trap number, and
     * r0..r3 the first four arguments, in the caller's (user) registers, which
     * are not banked in SVC and are therefore still live at handler entry.
     *   r12 <  0   Mach trap, index -r12
     *   r12 == 0   BSD indirect syscall, real number in r0
     *   r12 >  0   BSD syscall r12
     * (_unix_syscall corroborates: it reloads the number from savedstate+0x30,
     * i.e. regs->r12, and takes r0 as the number when it is zero.) */
    unsigned    sc_n, sc_dropped;
    struct { unsigned at; uint32_t r12, r[4], lr, spsr; } sc[512];

    /* IRQ entries, for the same reason FIQ entries are counted. */
    uint64_t    irq_n;

    /* distinct MMU faults */
    unsigned    fault_n;
    unsigned    fault_dropped;
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

    /* --- DIAGNOSTIC: FIQ arrival rate and the timer latch ------------------ */
    unsigned    fiq_n, fiq_last;
    uint64_t    fiq_instrs, fiq_longest;
    unsigned    fiq_storm_logged;

    /* --- DIAGNOSTIC: where the time actually goes -------------------------
     * A sampled profile keyed by function. When a boot runs a hundred million
     * instructions without reaching its next milestone, the question is not
     * "where did it crash" but "what is it doing instead", and a call-path
     * snapshot at an arbitrary stopping point does not answer that. */
    unsigned    prof_n, prof_dropped;
    struct { const char *fn; unsigned hits; } prof[1024];

    /*
     * The same samples keyed by EXACT pc, not by function.
     *
     * Needed because a prelinked kext has no symbol table (the kernelcache
     * builder strips LC_SYMTAB from every kext), so the function-level profile
     * can only say "com.apple.driver.AppleMBX" — true, and still not enough to
     * find the poll. The exact PC turns that into AppleMBX+0x122, which is an
     * address to disassemble. Open-addressed so a sample costs a hash and a
     * probe; capacity is a power of two and never grows.
     */
#define PCHASH 8192u
    unsigned    pc_n, pc_dropped;
    struct { uint32_t va; unsigned hits; } pc_hist[PCHASH];

    /* --- DIAGNOSTIC: the single hottest unmodelled MMIO page ---------------
     * One physical page absorbs ~2% of every instruction in a 200M boot. This
     * probe answers, for that page alone: which word offsets, from which PCs,
     * called from where, reading/writing what, and *when* -- so we can tell a
     * loop that eventually gives up from one that never does. */
    uint64_t    hot_now;                 /* instruction index, updated per step */
    uint64_t    hot_r[1024], hot_w[1024];/* per word-offset access counts */
    uint32_t    hot_last[1024];          /* last value read/written per offset */
    uint32_t    hot_firstw[1024];        /* first value ever written per offset */
    unsigned char hot_written[1024];
    unsigned    hot_site_n;
    struct { uint32_t pc, lr, off; bool wr; uint64_t n, first_at, last_at; } hot_site[64];
    unsigned    hot_log_n;               /* first N accesses, verbatim */
    struct { uint64_t at; uint32_t pc, lr, addr, val, r[6]; bool wr; unsigned bytes; } hot_log[80];
    unsigned    hot_tail_w; uint64_t hot_tail_n;   /* last N accesses, verbatim */
    struct { uint64_t at; uint32_t pc, lr, addr, val; bool wr; } hot_tail[64];
    uint64_t    hot_bucket[40];          /* when, over the whole run */
    uint64_t    hot_steps;               /* run length, for bucket scaling */
} G;

#define HOTPG 0x39a00000u

/* --- EXPERIMENT (-P): a candidate model for the power block at 0x39a00000 ---
 * Lives in the tool, not the core, so it can be tested without asserting
 * anything about hardware in the emulator proper. Semantics come entirely from
 * the traced AppleS5L8900XPowerController::start sequence:
 *   0x0C <- bits to clear in STATE      (write-1-to-clear)
 *   0x10 <- bits to set   in STATE      (write-1-to-set)
 *   0x14 -> STATE, read-only
 * plus plain storage for 0x00/0x04/0x08/0x24/0x28. */
/* Attribute one sample to an exact PC. Cheap: no name is resolved here, only
 * at report time. */
static void pc_sample(uint32_t va) {
    va &= ~1u;
    uint32_t h = va * 2654435761u;             /* Knuth multiplicative */
    for (unsigned probe = 0; probe < 64; probe++) {
        unsigned i = (h >> 13) + probe;
        i &= PCHASH - 1u;
        if (G.pc_hist[i].hits && G.pc_hist[i].va != va) continue;
        if (!G.pc_hist[i].hits) { G.pc_hist[i].va = va; G.pc_n++; }
        G.pc_hist[i].hits++;
        return;
    }
    G.pc_dropped++;                             /* never silently: reported */
}

/* The same, for user-mode PCs, in their own table so a handful of user
 * instructions is not buried under a hundred million kernel samples. */
static void upc_sample(uint32_t va) {
    va &= ~1u;
    uint32_t h = va * 2654435761u;
    for (unsigned probe = 0; probe < 64; probe++) {
        unsigned i = ((h >> 13) + probe) & (UPCHASH - 1u);
        if (G.upc_hist[i].hits && G.upc_hist[i].va != va) continue;
        if (!G.upc_hist[i].hits) { G.upc_hist[i].va = va; G.upc_n++; }
        G.upc_hist[i].hits++;
        return;
    }
    G.upc_dropped++;
}

/* Attribute one sample to a function, keeping the table small and exact. */
static void prof_sample(uint32_t va) {
    pc_sample(va);
    const char *nm = ksym_at(va);
    const char *bar = strchr(nm, '+');
    static char names[1024][96];
    char base[96];
    snprintf(base, sizeof base, "%.*s",
             bar ? (int)(bar - nm) : (int)strlen(nm), nm);
    for (unsigned i = 0; i < G.prof_n; i++)
        if (!strcmp(G.prof[i].fn, base)) { G.prof[i].hits++; return; }
    /* Never drop silently: a profile that quietly stops counting new
     * functions looks like "all the time is in early boot" when in fact the
     * later work simply had nowhere to go. */
    if (G.prof_n >= 1024) { G.prof_dropped++; return; }
    snprintf(names[G.prof_n], 96, "%s", base);
    G.prof[G.prof_n].fn = names[G.prof_n];
    G.prof[G.prof_n].hits = 1;
    G.prof_n++;
}

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

/* Record one access to the hot page in as much detail as we can afford. */
static void note_hot(uint32_t addr, uint32_t val, unsigned bytes, bool wr) {
    uint32_t pc = G.mach->cpu.r[15], lr = G.mach->cpu.r[14];
    unsigned wi = (addr & 0xfffu) >> 2;
    if (wr) G.hot_w[wi]++; else G.hot_r[wi]++;
    G.hot_last[wi] = val;
    if (wr && !G.hot_written[wi]) { G.hot_written[wi] = 1; G.hot_firstw[wi] = val; }

    unsigned i;
    for (i = 0; i < G.hot_site_n; i++)
        if (G.hot_site[i].pc == pc && G.hot_site[i].lr == lr &&
            G.hot_site[i].off == (addr & 0xfffu) && G.hot_site[i].wr == wr) break;
    if (i == G.hot_site_n && G.hot_site_n < 64) {
        G.hot_site[i].pc = pc; G.hot_site[i].lr = lr;
        G.hot_site[i].off = addr & 0xfffu; G.hot_site[i].wr = wr;
        G.hot_site[i].n = 0; G.hot_site[i].first_at = G.hot_now;
        G.hot_site_n++;
    }
    if (i < G.hot_site_n) { G.hot_site[i].n++; G.hot_site[i].last_at = G.hot_now; }

    if (G.hot_log_n < 80) {
        unsigned k = G.hot_log_n++;
        G.hot_log[k].at = G.hot_now; G.hot_log[k].pc = pc; G.hot_log[k].lr = lr;
        G.hot_log[k].addr = addr; G.hot_log[k].val = val;
        G.hot_log[k].wr = wr; G.hot_log[k].bytes = bytes;
        for (unsigned q = 0; q < 6; q++) G.hot_log[k].r[q] = G.mach->cpu.r[q];
    }
    {
        unsigned k = G.hot_tail_w;
        G.hot_tail[k].at = G.hot_now; G.hot_tail[k].pc = pc; G.hot_tail[k].lr = lr;
        G.hot_tail[k].addr = addr; G.hot_tail[k].val = val; G.hot_tail[k].wr = wr;
        G.hot_tail_w = (k + 1) % 64; G.hot_tail_n++;
    }
    if (G.hot_steps) {
        uint64_t b = G.hot_now * 40 / G.hot_steps;
        if (b > 39) b = 39;
        G.hot_bucket[b]++;
    }
}

static void spy(uint32_t addr, uint32_t val, unsigned bytes, bool wr) {
    if (is_ram(addr, bytes)) return;
    uint32_t pc = G.mach->cpu.r[15];
    note_dev_page(addr, pc, wr);
    if ((addr & ~0xfffu) == HOTPG) note_hot(addr, val, bytes, wr);
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

static uint32_t sr32(void *c, uint32_t a) {
    uint32_t v = G.inner.read32(c, a);
    spy(a, v, 4, false); return v;
}
static uint16_t sr16(void *c, uint32_t a) { uint16_t v = G.inner.read16(c, a); spy(a, v, 2, false); return v; }
static uint8_t  sr8 (void *c, uint32_t a) { uint8_t  v = G.inner.read8 (c, a); spy(a, v, 1, false); return v; }
static void sw32(void *c, uint32_t a, uint32_t v) {
    spy(a, v, 4, true);
    G.inner.write32(c, a, v);
}
static void sw16(void *c, uint32_t a, uint16_t v) { spy(a, v, 2, true); G.inner.write16(c, a, v); }
static void sw8 (void *c, uint32_t a, uint8_t  v) { spy(a, v, 1, true); G.inner.write8 (c, a, v); }

static void spy_install(s5l8900_t *m, uint32_t virt_base, uint32_t phys_base) {
    memset(&G, 0, sizeof G);
    G.mach  = m;
    G.inner = m->bus;
    m->bus.read32 = sr32; m->bus.read16 = sr16; m->bus.read8 = sr8;
    m->bus.write32 = sw32; m->bus.write16 = sw16; m->bus.write8 = sw8;
    for (unsigned i = 0; i < NM; i++)
        G.mile_pa[i] = MILE[i].va - virt_base + phys_base;
}

/*
 * Build the runtime milestone table: the hand-transcribed list verbatim, then
 * whichever of MILE_BYNAME the kernel's symbol table can resolve. Called after
 * ksyms_load and before spy_install, which reads NM.
 */
static void milestones_build(void) {
    for (unsigned i = 0; i < NMILE; i++) MILE[NM++] = MILESTONES[i];
    for (unsigned i = 0; i < NMILE_BYNAME; i++) {
        uint32_t va = ksym_value(MILE_BYNAME[i]);
        if (!va) { printf("  milestone %-28s UNRESOLVED — dropped\n",
                          MILE_BYNAME[i]); continue; }
        MILE[NM].name = MILE_BYNAME[i];
        MILE[NM].va   = va & ~1u;
        NM++;
    }
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
    } else {
        /* The table saturates early on a real boot — around instruction 116M.
         * Count what falls off: a silently truncated table reads as "these are
         * all the abort sites", which is exactly the wrong thing to believe
         * while diagnosing a wedge. The profile and hot-PC lists already report
         * their drops; this one did not. */
        G.fault_dropped++;
    }
}

/* ---------------------------------------------------------------------------
 * Naming the traps.
 *
 * Sparse tables rather than dense arrays: the point is that an unknown number
 * prints as a number, not as whatever entry happened to be at that index in a
 * table copied from the wrong kernel. Numbers are xnu-1357's
 * bsd/kern/syscalls.master and osfmk/kern/syscall_sw.c.
 * ------------------------------------------------------------------------- */
typedef struct { unsigned n; const char *name; } trapname_t;

static const trapname_t BSD_SYSCALL[] = {
    {0,"syscall(indirect)"},{1,"exit"},{2,"fork"},{3,"read"},{4,"write"},
    {5,"open"},{6,"close"},{7,"wait4"},{9,"link"},{10,"unlink"},{12,"chdir"},
    {13,"fchdir"},{14,"mknod"},{15,"chmod"},{16,"chown"},{18,"getfsstat"},
    {20,"getpid"},{23,"setuid"},{24,"getuid"},{25,"geteuid"},{26,"ptrace"},
    {27,"recvmsg"},{28,"sendmsg"},{29,"recvfrom"},{30,"accept"},
    {31,"getpeername"},{32,"getsockname"},{33,"access"},{34,"chflags"},
    {35,"fchflags"},{36,"sync"},{37,"kill"},{39,"getppid"},{41,"dup"},
    {42,"pipe"},{43,"getegid"},{46,"sigaction"},{47,"getgid"},
    {48,"sigprocmask"},{49,"getlogin"},{50,"setlogin"},{51,"acct"},
    {52,"sigpending"},{53,"sigaltstack"},{54,"ioctl"},{55,"reboot"},
    {56,"revoke"},{57,"symlink"},{58,"readlink"},{59,"execve"},{60,"umask"},
    {61,"chroot"},{65,"msync"},{66,"vfork"},{73,"munmap"},{74,"mprotect"},
    {75,"madvise"},{78,"mincore"},{79,"getgroups"},{80,"setgroups"},
    {81,"getpgrp"},{82,"setpgid"},{83,"setitimer"},{85,"swapon"},
    {86,"getitimer"},{89,"getdtablesize"},{90,"dup2"},{92,"fcntl"},
    {93,"select"},{95,"fsync"},{96,"setpriority"},{97,"socket"},
    {98,"connect"},{100,"getpriority"},{104,"bind"},{105,"setsockopt"},
    {106,"listen"},{111,"sigsuspend"},{116,"gettimeofday"},{117,"getrusage"},
    {118,"getsockopt"},{120,"readv"},{121,"writev"},{122,"settimeofday"},
    {123,"fchown"},{124,"fchmod"},{128,"rename"},{131,"flock"},{132,"mkfifo"},
    {133,"sendto"},{134,"shutdown"},{135,"socketpair"},{136,"mkdir"},
    {137,"rmdir"},{138,"utimes"},{139,"futimes"},{140,"adjtime"},
    {142,"gethostuuid"},{147,"setsid"},{151,"getpgid"},{152,"setprivexec"},
    {153,"pread"},{154,"pwrite"},{157,"statfs"},{158,"fstatfs"},
    {159,"unmount"},{161,"getfh"},{165,"quotactl"},{167,"mount"},
    {169,"csops"},{173,"waitid"},{176,"add_profil"},{180,"kdebug_trace"},
    {181,"setgid"},{182,"setegid"},{183,"seteuid"},{184,"sigreturn"},
    {185,"chud"},{188,"stat"},{189,"fstat"},{190,"lstat"},{191,"pathconf"},
    {192,"fpathconf"},{194,"getrlimit"},{195,"setrlimit"},
    {196,"getdirentries"},{197,"mmap"},{199,"lseek"},{200,"truncate"},
    {201,"ftruncate"},{202,"__sysctl"},{203,"mlock"},{204,"munlock"},
    {205,"undelete"},{216,"open_dprotected_np"},{220,"getattrlist"},
    {221,"setattrlist"},{222,"getdirentriesattr"},{223,"exchangedata"},
    {225,"searchfs"},{226,"delete"},{227,"copyfile"},{228,"fgetattrlist"},
    {229,"fsetattrlist"},{230,"poll"},{231,"watchevent"},{232,"waitevent"},
    {233,"modwatch"},{234,"getxattr"},{235,"fgetxattr"},{236,"setxattr"},
    {237,"fsetxattr"},{238,"removexattr"},{239,"fremovexattr"},
    {240,"listxattr"},{241,"flistxattr"},{242,"fsctl"},{243,"initgroups"},
    {244,"posix_spawn"},{245,"ffsctl"},{247,"nfsclnt"},{248,"fhopen"},
    {250,"minherit"},{266,"shm_open"},{267,"shm_unlink"},{268,"sem_open"},
    {269,"sem_close"},{270,"sem_unlink"},{271,"sem_wait"},{272,"sem_trywait"},
    {273,"sem_post"},{274,"sem_getvalue"},{275,"sem_init"},{276,"sem_destroy"},
    {277,"open_extended"},{278,"umask_extended"},{279,"stat_extended"},
    {280,"lstat_extended"},{281,"fstat_extended"},{282,"chmod_extended"},
    {283,"fchmod_extended"},{284,"access_extended"},{285,"settid"},
    {286,"gettid"},{293,"identitysvc"},{294,"shared_region_check_np"},
    {295,"shared_region_map_np"},{296,"vm_pressure_monitor"},
    {301,"__psynch_mutexwait"},{302,"__psynch_mutexdrop"},
    {327,"issetugid"},{328,"__pthread_kill"},{329,"__pthread_sigmask"},
    {330,"__sigwait"},{331,"__disable_threadsignal"},
    {332,"__pthread_markcancel"},{333,"__pthread_canceled"},
    {334,"__semwait_signal"},{336,"proc_info"},{337,"sendfile"},
    {338,"stat64"},{339,"fstat64"},{340,"lstat64"},{341,"stat64_extended"},
    {342,"lstat64_extended"},{343,"fstat64_extended"},
    {344,"getdirentries64"},{345,"statfs64"},{346,"fstatfs64"},
    {347,"getfsstat64"},{348,"__pthread_chdir"},{349,"__pthread_fchdir"},
    {350,"audit"},{351,"auditon"},{353,"getauid"},{354,"setauid"},
    {357,"getaudit_addr"},{358,"setaudit_addr"},{359,"auditctl"},
    {360,"bsdthread_create"},{361,"bsdthread_terminate"},{362,"kqueue"},
    {363,"kevent"},{364,"lchown"},{365,"stack_snapshot"},
    {366,"bsdthread_register"},{367,"workq_open"},{368,"workq_ops"},
    {371,"__mac_execve"},{372,"__mac_syscall"},{380,"__mac_execve"},
    {396,"read_nocancel"},{397,"write_nocancel"},{398,"open_nocancel"},
    {399,"close_nocancel"},{407,"fcntl_nocancel"},{408,"select_nocancel"},
    {409,"fsync_nocancel"},{412,"sigsuspend_nocancel"},
    {416,"__semwait_signal_nocancel"},{423,"__mac_mount"},
};

/* Mach traps live at NEGATIVE r12; the index is -r12. */
static const trapname_t MACH_TRAP[] = {
    {3,"ml_get_timebase (ARM fast trap)"},
    {26,"mach_reply_port"},{27,"thread_self_trap"},{28,"task_self_trap"},
    {29,"host_self_trap"},{31,"mach_msg_trap"},{32,"mach_msg_overwrite_trap"},
    {33,"semaphore_signal_trap"},{34,"semaphore_signal_all_trap"},
    {35,"semaphore_signal_thread_trap"},{36,"semaphore_wait_trap"},
    {37,"semaphore_wait_signal_trap"},{38,"semaphore_timedwait_trap"},
    {39,"semaphore_timedwait_signal_trap"},{43,"map_fd"},
    {44,"task_name_for_pid"},{45,"task_for_pid"},{46,"pid_for_task"},
    {48,"macx_swapon"},{49,"macx_swapoff"},{51,"macx_triggers"},
    {52,"macx_backing_store_suspend"},{53,"macx_backing_store_recovery"},
    {58,"pfz_exit"},{59,"swtch_pri"},{60,"swtch"},{61,"thread_switch"},
    {62,"clock_sleep_trap"},{89,"mach_timebase_info"},{90,"mach_wait_until"},
    {91,"mk_timer_create"},{92,"mk_timer_destroy"},{93,"mk_timer_arm"},
    {94,"mk_timer_cancel"},
};

static const char *trapname(const trapname_t *t, unsigned nt, unsigned num) {
    for (unsigned i = 0; i < nt; i++) if (t[i].n == num) return t[i].name;
    return NULL;
}

/*
 * The system calls the guest actually made.
 *
 * This is the report that says what to implement next: a boot that stops
 * making progress after five calls is not a mystery once the fifth call has a
 * name and arguments. Indirect calls (r12 == 0) are decoded through r0, which
 * is what _unix_syscall itself does.
 */
static void syscall_report(uint32_t virt_base, uint32_t phys_base) {
    (void)virt_base; (void)phys_base;
    printf("\n=== SYSTEM CALLS (caught at _fleh_swi; r12 = trap number) ===\n");
    printf("    %u captured%s\n", G.sc_n,
           G.sc_dropped ? " (table full, more dropped)" : "");
    if (!G.sc_n) {
        printf("    NONE — no SWI ever executed. Nothing in user mode asked the\n"
               "    kernel for anything, so pid 1 either never ran or never got\n"
               "    as far as its first call.\n");
        return;
    }
    for (unsigned i = 0; i < G.sc_n; i++) {
        int32_t num = (int32_t)G.sc[i].r12;
        const char *nm; char buf[64];
        bool thumb = (G.sc[i].spsr & ARM_CPSR_T) != 0;
        uint32_t upc = G.sc[i].lr - (thumb ? 2u : 4u);
        if (num < 0) {
            nm = trapname(MACH_TRAP, (unsigned)(sizeof MACH_TRAP / sizeof MACH_TRAP[0]),
                          (unsigned)(-num));
            if (!nm) { snprintf(buf, sizeof buf, "mach_trap #%d", -num); nm = buf; }
        } else {
            /* r12 == 0 is SYS_syscall: the real number is the first argument. */
            unsigned bn = num ? (unsigned)num : G.sc[i].r[0];
            nm = trapname(BSD_SYSCALL, (unsigned)(sizeof BSD_SYSCALL / sizeof BSD_SYSCALL[0]), bn);
            if (!nm) { snprintf(buf, sizeof buf, "bsd_syscall #%u", bn); nm = buf; }
        }
        printf("    #%-3u @%-11u r12 %-11d %-32s\n"
               "         args %08x %08x %08x %08x   from user pc %08x (%s, spsr %08x)\n",
               i, G.sc[i].at, num, nm,
               G.sc[i].r[0], G.sc[i].r[1], G.sc[i].r[2], G.sc[i].r[3],
               upc, thumb ? "Thumb" : "ARM", G.sc[i].spsr);
        /* An argument that points at a readable string is almost always the
         * path/name the call is about, and is the single most useful field. */
        for (unsigned a = 0; a < 4; a++) {
            const uint8_t *p = guest_ptr(G.sc[i].r[a], 4);
            if (!p) continue;
            unsigned printable = 0;
            for (unsigned q = 0; q < 8 && p[q]; q++)
                printable += (p[q] >= 0x20 && p[q] < 0x7f);
            if (printable >= 4) {
                char lbl[24]; snprintf(lbl, sizeof lbl, "       arg%u", a);
                print_guest_str(lbl, G.sc[i].r[a]);
            }
        }
    }
}

/*
 * Which privilege mode the guest spent its instructions in.
 *
 * The windowed half is the decisive one. If a late window contains ZERO
 * user-mode instructions, launchd is not running: it is parked inside the
 * kernel, and the thing to find is what it is parked on — not a user PC.
 */
static void mode_report(unsigned n, unsigned win_lo, unsigned win_hi) {
    static const struct { unsigned m; const char *nm; } MODES[] = {
        {ARM_MODE_USR,"USR (user)"},{ARM_MODE_FIQ,"FIQ"},{ARM_MODE_IRQ,"IRQ"},
        {ARM_MODE_SVC,"SVC (kernel)"},{ARM_MODE_ABT,"ABT (abort)"},
        {ARM_MODE_UND,"UND (undefined)"},{ARM_MODE_SYS,"SYS"},
    };
    bool windowed = (win_lo || win_hi != 0xffffffffu);
    printf("\n=== CPU MODE HISTOGRAM ===\n");
    printf("    %-18s %-22s %s\n", "mode", "whole run",
           windowed ? "WINDOW (-W)" : "");
    for (unsigned i = 0; i < sizeof MODES / sizeof MODES[0]; i++) {
        unsigned m = MODES[i].m;
        if (!G.mode_all[m] && !G.mode_win[m]) continue;
        printf("    %-18s %-12llu %5.1f%%    ", MODES[i].nm,
               (unsigned long long)G.mode_all[m],
               n ? 100.0 * (double)G.mode_all[m] / (double)n : 0.0);
        if (windowed)
            printf("%-12llu %5.1f%%", (unsigned long long)G.mode_win[m],
                   G.win_instrs ? 100.0 * (double)G.mode_win[m] / (double)G.win_instrs : 0.0);
        printf("\n");
    }
    printf("    %-18s %-12llu           ", "Thumb state",
           (unsigned long long)G.thumb_all);
    if (windowed) printf("%-12llu", (unsigned long long)G.thumb_win);
    printf("\n");
    if (windowed)
        printf("    window covered %llu instructions [%u,%u)\n",
               (unsigned long long)G.win_instrs, win_lo, win_hi);
    printf("    IRQ entries %llu\n", (unsigned long long)G.irq_n);
    if (!G.mode_all[ARM_MODE_USR])
        printf("    NO USER-MODE INSTRUCTION EVER RETIRED.\n");
    else if (windowed && !G.mode_win[ARM_MODE_USR])
        printf("    NO USER-MODE INSTRUCTION IN THE WINDOW — whatever is\n"
               "    spinning or sleeping, it is inside the kernel.\n");

    printf("\n=== HOTTEST USER-MODE PCs (every user instruction sampled) ===\n");
    if (!G.upc_n) { printf("    (none)\n"); return; }
    printf("    %u distinct user PCs%s\n", G.upc_n,
           G.upc_dropped ? " (hash full, incomplete)" : "");
    for (unsigned rank = 0; rank < 24; rank++) {
        uint64_t best = 0; unsigned bi = UPCHASH;
        for (unsigned i = 0; i < UPCHASH; i++)
            if (G.upc_hist[i].hits > best) { best = G.upc_hist[i].hits; bi = i; }
        if (bi == UPCHASH || !best) break;
        printf("    0x%08x  %llu\n", G.upc_hist[bi].va, (unsigned long long)best);
        G.upc_hist[bi].hits = 0;
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
            "          [-d <devicetree.bin>] [-c <cmdline>] [-a] [-M] [-F] [-g]\n"
            "          [-r <ramdisk.img>] [-R <ram-MB>] [-X phys|virt|<addr>]\n"
            "          [-D <node/path>:<prop>=<value>] ...\n"
            "          [--snapshot-at <insn> <file>] ... [--restore <file>]\n"
            "  --snapshot-at <insn> <file>\n"
            "      write the whole machine to <file> the moment the retired-\n"
            "      instruction counter reaches <insn>. Up to 8 checkpoints; the\n"
            "      count is the machine's own (mach.cpu.cycles), which is part of\n"
            "      the snapshot, so the trigger point is ABSOLUTE — a run that\n"
            "      restored at 200000000 still fires --snapshot-at 300000000 at\n"
            "      the same instruction a run from zero would have.\n"
            "  --restore <file>\n"
            "      start from a saved machine instead of from the kernel entry\n"
            "      point, so a long boot is paid for once. The machine is built\n"
            "      exactly as for a fresh boot and then overwritten, which means\n"
            "      A RESTORED RUN STILL NEEDS THE SAME -d / -r / -R (and -p/-V)\n"
            "      AS THE RUN THAT SAVED IT: snapshot_load refuses with\n"
            "      SNAP_ERR_GEOMETRY unless the pre-restore setup allocated the\n"
            "      same-size RAM at the same base. -c and the kernel image do not\n"
            "      change geometry, but pass the same ones anyway — the report\n"
            "      header echoes them and a mismatched header describes a machine\n"
            "      that is not the one being run.\n"
            "      NOTE the host-side diagnostics below (trace ring, milestone\n"
            "      hit counts, sampled profile, hot-page log) are NOT machine\n"
            "      state and are not restored; they start fresh and therefore\n"
            "      cover only the window since the restore. Instruction INDICES\n"
            "      are absolute either way. Use tools/snapboot for a report that\n"
            "      is a pure function of the machine.\n"
            "  -D  patch a 4-byte device-tree property in the in-memory copy\n"
            "      (empty path == root), e.g. -D cpus/cpu0:timebase-frequency=6000000\n"
            "  -r  load a raw disk image into DRAM, publish it as the RAMDisk\n"
            "      entry of /chosen/memory-map, and append rd=md0 to the cmdline\n"
            "  --fstab <line>  what to write over the guest's /private/etc/fstab\n"
            "      record in the loaded RAM disk (default\n"
            "      \"/dev/md0 / hfs rw,update 0 1\"). The stock record names\n"
            "      /dev/disk0s1 and /dev/disk0s2, which only exist behind\n"
            "      AppleNANDFTL + IOFlashPartitionScheme; this VM has no NAND-\n"
            "      backed disk0, so launchd's fsck fails and it halts the\n"
            "      machine. The image on disk is never modified.\n"
            "  --keep-fstab    leave the stock record alone (reproduces the halt)\n"
            "  --grow <MB>  free space to give the guest by growing the HFS+\n"
            "      volume in the loaded RAM disk (default 32, 0 disables). The\n"
            "      stock system dmg is sized exactly to its contents —\n"
            "      freeBlocks is 0 — because on hardware everything writable\n"
            "      lives on disk0s2, which this machine does not have. Without\n"
            "      this, launchd, the daemons and SpringBoard cannot create a\n"
            "      single file. Costs the guest's free page pool 1:1 (the RAM\n"
            "      disk is static memory below topOfKernelData), and is capped\n"
            "      by the allocation file at a 512 MB volume. The image on disk\n"
            "      is never modified.\n"
            "  -Y  EXPERIMENT: put the RAM disk at the BOTTOM of DRAM, below\n"
            "      the kernel, instead of above it. Both are below\n"
            "      topOfKernelData and so equally protected, but below the\n"
            "      kernel the disk stops pushing that line up and the free\n"
            "      page pool grew by its whole size in historical 768 MB\n"
            "      experiments. Those old commands are now rejected: the\n"
            "      corrected model reserves NOR at 0x28000000, exactly after\n"
            "      the supported 512 MB DRAM window.\n"
            "      It needs -V to open a gap under the kernel, and THAT is\n"
            "      what does not work yet: with -V below 0xc0000000 the boot\n"
            "      reaches \"BSD root: md0\" and then goes idle without ever\n"
            "      reaching _load_init_program. Headroom on paper only.\n"
            "  -L  print the prelinked kext load map (bundle id, load address,\n"
            "      size) read out of __PRELINK_INFO, then exit without booting\n"
            "  -R  guest DRAM size in MB (default 128, the iPhone1,2 fitment)\n"
            "  -X  what to write as the RAMDisk address: 'phys' (the physical\n"
            "      address it was loaded at), 'virt' (the ml_static_ptovirt form,\n"
            "      the default), or a literal address to use as a sentinel\n"
            "  -b  boot_args Revision field (default 1)\n"
            "  -M  do not synthesise /memory reg from the RAM layout\n"
            "  -F  reserve a 320x480x32 Boot_Video buffer, seed CLCD window 0\n"
            "      exactly as iBoot leaves it, and patch the N82 Merlot panel\n"
            "      ID when -d is present. v_display stays 0 so the kernel can\n"
            "      draw boot text; remove serial=1 from -c to route it there.\n"
            "  -g  graphics experiment: implies -F, sets v_display=1, and\n"
            "      leaves the unmodelled MBX driver matched (it may stall).\n"
            "  -W  <lo>[:<hi>] restrict the sampled profile / hot-PC table /\n"
            "      per-kext attribution to instructions in [lo,hi). A whole-run\n"
            "      profile of a boot that STALLS describes the boot, not the\n"
            "      stall; a window that starts after the last milestone\n"
            "      characterises what is actually spinning.\n"
            "  -Z  <n> print pc/mode/symbol every n instructions\n"
            "  -T  how many trace lines to print at the first data abort\n"
            "  -K  disable the post-load kernel patches (see the kpatch table)\n"
            "  -A  DIAGNOSTIC SHIM, not a fix: after an exception return that\n"
            "      resumes in Thumb state, undo the word alignment the core\n"
            "      applies to the resume address. Use it to confirm that a\n"
            "      failure is caused by that alignment, never to 'work'.\n"
            "  -P  EXPERIMENT, not a fix: model the S5L8900X power block at\n"
            "      0x39a00000 inside this tool (0x0C write-1-to-clear,\n"
            "      0x10 write-1-to-set, 0x14 = state). Proves what the\n"
            "      AppleS5L8900XPowerController poll is waiting for. The real\n"
            "      model belongs in the core; this switch only demonstrates it.\n",
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
    uint32_t v_display = 0;   /* 0 = kernel text console draws; 1 = defer to IOMFB */
    bool want_mbx = false;    /* -g keeps the (unmodelled, hang-prone) MBX GPU driver */
    bool want_sha1hw = false; /* -S keeps the (unmodelled) SHA-1 engine matched */
    bool no_kpatch = false;   /* -K disables the post-load kernel patches */
    bool want_kextmap = false;/* -L prints the kext load map and exits */
    /* -A: from outside the core, undo the word-alignment the interpreter
     * applies when an exception return resumes in Thumb state. This is a
     * PROOF SHIM for a suspected core bug, not a fix. */
    /*
     * boot_args header. MEASURED, not guessed: pe_identify_machine() at
     * 0xc01a7f22 does `ldrh r3,[r0,#2]; cmp r3,#6` and panics
     * "pe_identify_machine: Epoch Mismatch" on anything else, so the halfword
     * at boot_args+2 must be 6. Revision (+0) is not checked here; 1 works.
     * Override with -b/-w to re-probe (-w 2 reproduces the original panic).
     */
    unsigned ba_rev = 1, ba_ver = 6;

    /* -r / -R / -X: the RAM-disk root. */
    const char *rdpath  = NULL;
    uint32_t    ram_size = 128u << 20;     /* iPhone1,2 fitment; -R overrides */
    /*
     * Which form of the address goes into the device-tree property.
     *
     * RD_ADDR_VIRT is the default and the answer settled by experiment (see the
     * report next to this flag's use): nothing between the property and the
     * bcopy translates, and the copy is a plain dereference, so the value has
     * to be a kernel virtual address. 'phys' and a literal sentinel exist so
     * that conclusion can be re-derived rather than taken on faith.
     */
    enum { RD_ADDR_VIRT, RD_ADDR_PHYS, RD_ADDR_LITERAL } rd_form = RD_ADDR_VIRT;
    uint32_t rd_literal = 0;

    /*
     * --fstab / --keep-fstab: see rd_rewrite_fstab() above for why this exists.
     *
     * Default ON, because with the stock record launchd's bootstrap ALWAYS ends
     * in reboot(RB_HALT) on this machine — a default of "off" would mean the
     * documented boot command never gets past launchd. The rewrite is printed
     * on every run so it is never silent, and --keep-fstab restores the stock
     * record for anyone re-deriving the failure.
     *
     * The default line is chosen from what launchctl and /sbin/fsck actually
     * do with it:
     *   - pass 1 makes /sbin/fsck -p check the volume that really exists, so
     *     the boot still exercises Apple's own fsck_hfs against md0 rather
     *     than skipping the check the hardware would have done;
     *   - rw,update is what turns launchctl's "mount -vat nonfs" into a
     *     remount of / read-write. xnu mounts every root MNT_RDONLY
     *     (vfs_rootmountalloc), and md0 is a writable memory device, so the
     *     update is the only thing standing between us and a writable
     *     /private/var — which SpringBoard needs and which, on hardware, would
     *     have come from disk0s2.
     */
    const char *fstab_line  = "/dev/md0 / hfs rw,update 0 1";
    bool        fstab_fixup = true;

    /*
     * --grow <MB>: free space to give the guest, by growing the HFS+ volume in
     * the loaded RAM disk. See rd_grow_volume() above for the format work; this
     * is the size question.
     *
     * The stock volume has freeBlocks == 0, so ANY nonzero amount is the
     * qualitative change. The number is a trade against the free page pool,
     * because the RAM disk sits below topOfKernelData and every megabyte of it
     * is a megabyte the guest VM never gets: at the documented -R 512 the pool
     * is 90.9 MB before this and 58.9 MB after.
     *
     * 32 MB is picked as comfortably more than a boot needs and visibly less
     * than the pool can spare. What actually gets written on the way to
     * SpringBoard is /private/var/log/asl, /private/var/db (launchd.db,
     * timezone), /private/var/root/Library/Lockdown, /private/var/preferences
     * and the mobile user's caches — plists and logs, single-digit megabytes.
     * (The whole /private/var skeleton is already present on the system dmg:
     * 16 entries including db, log, mobile, root, preferences, run and tmp, so
     * nothing has to be created wholesale first.) On hardware /private/var is
     * a multi-gigabyte disk0s2 and none of this is a constraint; here it is,
     * so raise it deliberately rather than by default. --grow 0 disables.
     */
    uint32_t rd_grow_mb = 32;

    /*
     * -Y: put the RAM disk at the BOTTOM of DRAM, below the kernel image.
     *
     * topOfKernelData is a single LINE, not a list: everything below it is the
     * kernel's static pre-loaded data and is never handed to the VM. So a RAM
     * disk placed below the kernel's load address is exactly as protected as
     * one placed above it — and it costs the free page pool nothing, because
     * topOfKernelData then only has to clear the kernel's own image instead of
     * the kernel's image plus 413 MB of root filesystem.
     *
     * It only buys anything with -V, which is what decides where the kernel
     * lands physically (phys = vmaddr - virt_base + phys_base): the RAM disk
     * has to fit in [phys_base, kernel load address), and at the default
     * -V 0xc0000000 that gap is zero. The arithmetic works, MEASURED:
     *
     *   -R 512 (default)                       445 MB disk,  58.93 MB pool
     *   -V 0xa4000000 -R 768 -Y                445 MB disk, 312.14 MB pool
     *   -V 0xa0000000 -R 768 -Y --grow 100     512 MB disk, 248.14 MB pool
     *
     * Those 768 MB rows are retained as historical measurements only. They
     * predate the corrected modelled NOR window at 0x28000000 and the strict
     * RAM/device overlap check. The largest accepted window is now 512 MB:
     * [0x08000000,0x28000000). The HFS allocation file independently caps the
     * volume at 512 MB as well.
     *
     * EXPERIMENT, NOT YET A WORKING BOOT, and the reason is -V rather than -Y:
     * both -V rows above reach "BSD root: md0" and then go idle without ever
     * reaching _load_init_program, where the default -V 0xc0000000 execs
     * launchd at ~225 M instructions. That is the failure the -V note further
     * down predicted — VM_MIN_KERNEL_ADDRESS is compiled into this kernel, and
     * below it the pmap is looking at what it thinks is user space. Until that
     * is understood, the headroom here is arithmetic rather than usable, which
     * is exactly why -Y is off by default.
     */
    bool rd_low = false;

    /* -W <lo>[:<hi>] profile window, -Z <n> heartbeat. Defaults are "the whole
     * run" and "silent", so neither changes an existing invocation. */
    unsigned win_lo = 0, win_hi = 0xffffffffu, heartbeat = 0;
    /* Filled once the symbol table is loaded; 0 disables syscall capture. */
    uint32_t fleh_swi_va = 0, fleh_swi_pa = 0;

    /* -D <path>:<prop>=<value> overrides / adds a device-tree patch, so the
     * frequencies can be probed from the shell instead of by rebuilding. */
    struct { const char *path, *prop; uint32_t val; } dtov[32];
    unsigned ndtov = 0;
    char dtbuf[32][96];
    bool patch_memnode = true;
    unsigned ktail = 512;              /* -T n: trace lines to print on abort */

    /*
     * Snapshot / restore (core/include/snapshot.h).
     *
     * --restore <file>            start from a saved machine instead of from the
     *                             kernel entry point.
     * --snapshot-at <insn> <file> save the whole machine the moment the retired-
     *                             instruction counter reaches <insn>.
     *
     * Both are keyed on mach.cpu.cycles, the machine's OWN retired-instruction
     * count, which is part of the snapshot. That is what makes a trigger point
     * absolute across processes: a run restored at 200,000,000 sees cycles ==
     * 200000000 and will still fire a checkpoint requested for 300,000,000 at
     * exactly the instruction the original run would have.
     */
    const char *restore_path = NULL;
    struct { uint64_t at; const char *path; } snaps[8];
    unsigned nsnaps = 0;

    /* Walk the arguments one at a time: pair-stepping breaks as soon as a
     * single-argument flag like -a appears. */
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "-a")) { stop_on_abort = true; continue; }
        if (!strcmp(argv[i], "-F")) { want_fb = true; continue; }
        if (!strcmp(argv[i], "-g")) { want_fb = true; v_display = 1; want_mbx = true; continue; }  /* defer to IOMFB, keep MBX */
        if (!strcmp(argv[i], "-S")) { want_sha1hw = true; continue; }  /* keep the SHA-1 nub matched */
        if (!strcmp(argv[i], "-K")) { no_kpatch = true; continue; }  /* no kernel patches */
        if (!strcmp(argv[i], "-M")) { patch_memnode = false; continue; }
        if (!strcmp(argv[i], "-L")) { want_kextmap = true; continue; }
        if (!strcmp(argv[i], "--keep-fstab")) { fstab_fixup = false; continue; }
        if (!strcmp(argv[i], "-Y")) { rd_low = true; continue; }
        /* Three-argument flag: must be recognised before the two-argument
         * guard below, which would otherwise stop the walk on the last pair. */
        if (!strcmp(argv[i], "--snapshot-at") && i + 2 < argc) {
            if (nsnaps < 8) {
                snaps[nsnaps].at   = strtoull(argv[i + 1], NULL, 0);
                snaps[nsnaps].path = argv[i + 2];
                nsnaps++;
            }
            i += 2; continue;
        }
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
        if (!strcmp(argv[i], "-W")) {
            const char *v = argv[++i];
            char *colon = NULL;
            win_lo = (unsigned)strtoul(v, &colon, 0);
            win_hi = (colon && *colon == ':') ? (unsigned)strtoul(colon + 1, NULL, 0)
                                              : 0xffffffffu;
            continue;
        }
        if      (!strcmp(argv[i], "-Z")) heartbeat = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-p")) phys_base = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--restore")) restore_path = argv[++i];
        else if (!strcmp(argv[i], "-V")) virt_base = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-n")) steps     = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-T")) ktail     = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-d")) dtpath    = argv[++i];
        else if (!strcmp(argv[i], "-c")) cmdline   = argv[++i];
        else if (!strcmp(argv[i], "-b")) ba_rev    = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-w")) ba_ver    = (unsigned)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-r")) rdpath    = argv[++i];
        else if (!strcmp(argv[i], "--fstab")) fstab_line = argv[++i];
        else if (!strcmp(argv[i], "--grow")) rd_grow_mb = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-R")) ram_size  = (uint32_t)strtoul(argv[++i], NULL, 0) << 20;
        else if (!strcmp(argv[i], "-X")) {
            const char *v = argv[++i];
            if      (!strcmp(v, "phys")) rd_form = RD_ADDR_PHYS;
            else if (!strcmp(v, "virt")) rd_form = RD_ADDR_VIRT;
            else { rd_form = RD_ADDR_LITERAL; rd_literal = (uint32_t)strtoul(v, NULL, 0); }
        }
    }

    /*
     * Clamp memSize to the kernel's static-map ceiling — the fix for the
     * "sleh_abort at interrupt context" panic in early VM bring-up.
     *
     * arm_vm_init hardcodes the start of the DYNAMIC kernel address space at
     * virtual_avail = 0xe0000000 (MEASURED: `movs r0,#0xe0; lsls r0,#24` at
     * arm_vm_init+0x18c, passed to pmap_bootstrap). So the kernel's identity /
     * physical-linear window is fixed at [gVirtBase, 0xe0000000) = 512 MB, and
     * everything pmap_steal_memory hands out (zdata, the bootstrap zones) lives
     * at 0xe0000000 and up.
     *
     * zone_bootstrap()'s zcram runs BEFORE pmap_init() (the vm_mem_bootstrap
     * order is vm_page_bootstrap -> zone_bootstrap -> ... -> pmap_init), and for
     * every element it calls zone_virtual_addr(), which treats any address in
     * [gVirtBase, gVirtBase + mem_size) as identity-mapped and dereferences
     * pv_head_table[(phys - vm_first_phys) >> 12]. Before pmap_init that table
     * is all zero and vm_first_phys is 0, so the entry is NULL and [NULL+8]
     * faults — the null-zone data abort at _zone_virtual_addr+0x2c.
     *
     * The ONLY thing that keeps this from firing is zone_virtual_addr's
     * early-out: it returns the address untouched when (addr - gVirtBase) >=
     * mem_size. Real S5L8900/S5L8920 devices ship <=256 MB, so mem_size never
     * reaches 0xe0000000 and the steal region always early-outs. With mem_size
     * > 512 MB the window swallows the steal region and the boot panics.
     *
     * The kernel cannot address DRAM past 0xe0000000 anyway, so advertising more
     * is pointless as well as fatal. Cap it. (This is independent of where the
     * RAM disk sits: virtual_avail is 0xe0000000 for a 12 MB or a 413 MB disk
     * alike — placement does not change it, MEASURED.)
     */
    /*
     * The ceiling is 0xe0000000 - gVirtBase, NOT a constant 512 MB. It reads as
     * 512 only because gVirtBase is 0xc0000000, and gVirtBase is a boot_args
     * field we choose (-V), not something the kernel hardcodes the way it
     * hardcodes virtual_avail. With -V 0xb0000000 the kernel-side arithmetic
     * alone would permit 768 MB, but the SoC model correctly rejects that
     * physical map because it reaches the NOR window at 0x28000000. Keep the
     * cap relative to gVirtBase so lower-base experiments at or below the
     * physical 512 MB ceiling remain testable.
     *
     * That experiment is NOT known to work and must be run before it is
     * believed — if VM_MIN_KERNEL_ADDRESS is compiled into this kernel as
     * 0xc0000000 then addresses below it are user addresses and the boot will
     * fail in a confusing way rather than an obvious one. -V is how you find
     * out; this cap is just no longer the thing preventing you from asking.
     */
    const uint32_t KERN_VIRTUAL_AVAIL = 0xe0000000u;  /* MEASURED, arm_vm_init+0x18c */
    const uint32_t KERN_MEMSIZE_MAX   = KERN_VIRTUAL_AVAIL - virt_base;
    if (ram_size > KERN_MEMSIZE_MAX) {
        fprintf(stderr,
                "note: capping guest RAM %u MB -> %u MB — xnu-1357 arm_vm_init fixes\n"
                "      virtual_avail at 0x%08x, so mem_size above (that - gVirtBase)\n"
                "      makes zone_bootstrap's early zone_virtual_addr() fault on an\n"
                "      uninitialised pv_head_table (panic: sleh_abort at interrupt\n"
                "      context). The kernel cannot use DRAM past that line anyway.\n",
                ram_size >> 20, KERN_MEMSIZE_MAX >> 20, KERN_VIRTUAL_AVAIL);
        ram_size = KERN_MEMSIZE_MAX;
    }

    /* Needed by ksym_at() to fold a pre-MMU physical PC back to its virtual
     * address, so set them as soon as they are known — before the first name
     * is ever printed, not after the machine exists. */
    g_virt_base = virt_base; g_phys_base = phys_base;

    size_t len = 0;
    uint8_t *img = slurp(argv[1], &len);
    if (!img) return 1;

    macho_t m;
    macho_status_t mst = macho_parse(img, len, &m);
    if (mst != MACHO_OK) { fprintf(stderr, "macho: %s\n", macho_strerror(mst)); return 2; }
    if (!m.has_entry)    { fprintf(stderr, "no entry point in the image\n"); return 2; }

    /*
     * Symbols BEFORE the machine: -L only needs the image, and refusing to
     * spin up a 512 MB machine to print a table keeps the flag usable while a
     * long boot is running.
     */
    ksyms_load(&KS, img, len);
    report_symbol_sources();
    milestones_build();
    /* _fleh_swi is the one address the syscall capture needs; take it from the
     * symbol table rather than the transcribed milestone so a different kernel
     * build cannot silently point the probe at the wrong code. */
    fleh_swi_va = ksym_value("_fleh_swi") & ~1u;
    fleh_swi_pa = fleh_swi_va - virt_base + phys_base;
    if (want_kextmap) {
        printf("\n");
        dump_kext_map();
        ksyms_free(&KS);
        free(img);
        return 0;
    }

    /* Enough RAM to hold the kernel plus room for its page tables and heap.
     * 128 MB is what an iPhone1,2 actually has; -R raises it when a large
     * RAM disk has to live in DRAM alongside everything else. */
    s5l8900_t mach;
    if (!s5l8900_init(&mach, phys_base, ram_size)) { fprintf(stderr, "init failed\n"); return 1; }
    mach.trace_devices = true;
    g_mach = &mach; g_virt_base = virt_base; g_phys_base = phys_base;

    boot_range_t dram_range, kernel_range;
    uint64_t kernel_begin64, kernel_end64;
    if (!boot_range_make(&dram_range, "DRAM", phys_base, ram_size, true) ||
        m.vm_low < virt_base || m.vm_high < m.vm_low ||
        !add_u64(phys_base, (uint64_t)m.vm_low - virt_base, &kernel_begin64) ||
        !add_u64(phys_base, (uint64_t)m.vm_high - virt_base, &kernel_end64) ||
        !boot_range_make(&kernel_range, "kernel", kernel_begin64,
                         kernel_end64 - kernel_begin64, true) ||
        !boot_layout_validate(&dram_range, &kernel_range, 1)) {
        fprintf(stderr,
                "layout: kernel virtual span [0x%08x,0x%08x) cannot be "
                "mapped safely from virtual base 0x%08x\n",
                m.vm_low, m.vm_high, virt_base);
        return 1;
    }

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
        uint64_t pa64, file_end64;
        if (!add_u64(phys_base, (uint64_t)s->vmaddr - virt_base, &pa64) ||
            !add_u64(pa64, s->filesize, &file_end64) ||
            pa64 > UINT32_MAX || pa64 < kernel_range.begin ||
            file_end64 > kernel_range.end || file_end64 > dram_range.end) {
            fprintf(stderr, "layout: kernel segment %s has an unsafe physical span\n",
                    s->name);
            return 1;
        }
        uint32_t pa = (uint32_t)pa64;
        s5l8900_load(&mach, pa, img + s->fileoff, s->filesize);
        printf("  load %-16s vm 0x%08x -> pa 0x%08x  %u bytes\n",
               s->name, s->vmaddr, pa, s->filesize);
    }

    /*
     * Kernel patches applied in guest RAM after load — what a jailbroken iBoot
     * would do to the kernel image before jumping to it, and legitimate here
     * because the guest is ours to modify (jailbreaking the guest is a project
     * goal). Each patch names the byte it expects to find so a wrong offset or
     * a different kernel build fails loudly instead of corrupting code.
     *
     * IORTC wait: bsd_init calls IOKitInitializeTime, which does
     *   waitForService(resourceMatching("IORTC"), &{tv_sec=30}) before
     *   vfsinit / IOFindBSDRoot. The resource is published only by
     *   ApplePCF50635PMURTC, and we do not model the PCF50635 PMU on I2C, so it
     *   is never published and the boot thread sleeps the full 30 s. At our
     *   timebase that is ~12.4 billion instructions, and running it out does not
     *   even help (the config threads block permanently in driver start()s
     *   awaiting unmodelled hardware, so the post-timeout path re-stalls). Zero
     *   the tv_sec immediate (movs r3,#0x1e -> movs r3,#0) so the wait returns
     *   at once; the boot then reaches IOFindBSDRoot and mounts md0.
     *   The proper fix is to model the PMU RTC so it publishes IORTC; until then
     *   this one byte is the difference between "idle forever" and "BSD root".
     */
    if (!no_kpatch) {
        struct { uint32_t va; uint8_t want, set; const char *why; } kp[] = {
            { 0xc0175b3eu, 0x1e, 0x00, "IORTC wait tv_sec 30->0 (reach IOFindBSDRoot)" },
        };
        for (unsigned i = 0; i < sizeof kp / sizeof kp[0]; i++) {
            uint32_t pa = kp[i].va - virt_base + phys_base;
            uint8_t  got = mach.bus.read8(mach.bus.ctx, pa);
            if (got != kp[i].want) {
                printf("  kpatch SKIP %08x: found %02x expected %02x (%s)\n",
                       kp[i].va, got, kp[i].want, kp[i].why);
                continue;
            }
            mach.bus.write8(mach.bus.ctx, pa, kp[i].set);
            printf("  kpatch %08x: %02x -> %02x  %s\n",
                   kp[i].va, kp[i].want, kp[i].set, kp[i].why);
        }
    }

    uint64_t entry_pa64;
    if (m.entry < virt_base ||
        !add_u64(phys_base, (uint64_t)m.entry - virt_base, &entry_pa64) ||
        entry_pa64 > UINT32_MAX || entry_pa64 < kernel_range.begin ||
        entry_pa64 >= kernel_range.end) {
        fprintf(stderr, "layout: kernel entry 0x%08x is outside the kernel span\n",
                m.entry);
        return 1;
    }
    uint32_t entry_pa = (uint32_t)entry_pa64;
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
    uint32_t dt_pa = 0, dt_len = 0;

    /* The tree has to be read before the layout is fixed, because the RAMDisk
     * entry written into it names an address that depends on where everything
     * else landed — and the tree's own length is part of that arithmetic. */
    size_t dt_n = 0;
    uint8_t *dt = dtpath ? slurp(dtpath, &dt_n) : NULL;
    if (dtpath && !dt) {
        fprintf(stderr, "devicetree: cannot read %s\n", dtpath);
        return 1;
    }
    if (dt_n > UINT32_MAX) {
        fprintf(stderr, "devicetree: %llu-byte input exceeds the 32-bit boot_args field\n",
                (unsigned long long)dt_n);
        free(dt);
        return 1;
    }
    if (dt) dt_len = (uint32_t)dt_n;

    /* Learn the RAM-disk size now, but do not allocate or read it yet. It is
     * streamed into its final guest-DRAM address after the layout is checked. */
    size_t rd_n = 0;
    uint8_t *rd = NULL;
    streamed_file_t rd_source;
    memset(&rd_source, 0, sizeof rd_source);
    if (rdpath && !streamed_file_open(rdpath, &rd_source, &rd_n)) {
        fprintf(stderr, "ramdisk: cannot size %s\n", rdpath);
        return 1;
    }

    /*
     * Reserve a linear framebuffer and advertise it in Boot_Video. With
     * v_baseAddr left at 0 the kernel's PE_create_console reads address 0,
     * finds no framebuffer, and the video console silently goes nowhere — which
     * is why no boot text ever appeared. A real buffer lets the kernel render
     * its console into memory we can look at.
     */
    const uint32_t FB_W = N82_FB_WIDTH, FB_H = N82_FB_HEIGHT;
    const uint32_t FB_BPP = N82_FB_BPP;
    const uint32_t fb_bytes = want_fb ? N82_FB_BYTES : 0u;

    uint64_t dt_pa64, ba_pa64;
    boot_range_t dt_range, ba_range;
    if (!align_u64(kernel_range.end, 0x1000u, &dt_pa64) ||
        dt_pa64 > UINT32_MAX ||
        !boot_range_make(&dt_range, "device tree", dt_pa64, dt_n, dt != NULL) ||
        !align_u64(dt_range.end, 0x1000u, &ba_pa64) ||
        ba_pa64 > UINT32_MAX ||
        !boot_range_make(&ba_range, "boot_args page", ba_pa64, 0x1000u, true)) {
        fprintf(stderr,
                "layout: kernel, device tree, and boot_args do not fit in "
                "32-bit space\n");
        streamed_file_close(&rd_source);
        return 1;
    }
    dt_pa = (uint32_t)dt_pa64;
    uint32_t ba_pa = (uint32_t)ba_pa64;

    /*
     * The RAM disk goes immediately after boot_args and BELOW topOfKernelData.
     *
     * That placement is not cosmetic. topOfKernelData is the kernel's statement
     * of where its pre-loaded data ends; everything above it is free DRAM that
     * the VM will hand out and overwrite. A RAM disk parked above that line is
     * a root filesystem being scribbled on by the page allocator. iBoot puts it
     * below the line for exactly this reason, so we do too.
     *
     * mdevadd takes base and size as PAGE NUMBERS (addr >> 12), so both have to
     * be page-multiples or the disk is silently truncated / misaligned.
     */
    /* -Y moves it BELOW the kernel instead (see the flag's note): still below
     * topOfKernelData, still static, but no longer pushing that line — and
     * therefore the free page pool — up by the size of the root filesystem. */
    uint64_t rd_pa64, capacity64 = 0, rd_reserve_len64 = 0;
    if (rdpath &&
        (!add_u64((uint64_t)rd_n, (uint64_t)rd_grow_mb << 20,
                  &capacity64) ||
         capacity64 > SIZE_MAX || capacity64 > UINT32_MAX ||
         !align_u64(capacity64, 0x1000u, &rd_reserve_len64))) {
        fprintf(stderr, "ramdisk: source plus requested growth is too large\n");
        streamed_file_close(&rd_source);
        return 1;
    }
    if (rd_low) {
        rd_pa64 = phys_base;
    } else if (!align_u64(ba_range.end, 0x1000u, &rd_pa64)) {
        fprintf(stderr, "layout: RAM-disk address overflow\n");
        streamed_file_close(&rd_source);
        return 1;
    }

    boot_range_t rd_range;
    if (rd_pa64 > UINT32_MAX ||
        !boot_range_make(&rd_range, "RAM disk reserve", rd_pa64,
                         rd_reserve_len64, rdpath != NULL)) {
        fprintf(stderr, "layout: RAM-disk span exceeds 32-bit physical space\n");
        streamed_file_close(&rd_source);
        return 1;
    }
    uint32_t rd_pa = (uint32_t)rd_pa64;
    size_t capacity = (size_t)capacity64;

    uint64_t fb_start64 = 0, planned_static_end64 = 0;
    boot_range_t fb_range;
    if (want_fb) {
        if ((uint64_t)ram_size < fb_bytes) {
            fprintf(stderr,
                    "framebuffer: %u bytes do not fit in %u bytes of DRAM\n",
                    fb_bytes, ram_size);
            streamed_file_close(&rd_source);
            return 1;
        }
        fb_start64 = (dram_range.end - fb_bytes) & ~UINT64_C(0xfff);
    }
    if (!boot_range_make(&fb_range, "framebuffer", fb_start64, fb_bytes,
                         want_fb)) {
        fprintf(stderr, "framebuffer: physical span overflow\n");
        streamed_file_close(&rd_source);
        return 1;
    }

    boot_range_t layout_ranges[] = {
        kernel_range, dt_range, ba_range, rd_range, fb_range
    };
    if (!boot_layout_validate(&dram_range, layout_ranges,
                              sizeof layout_ranges / sizeof layout_ranges[0])) {
        streamed_file_close(&rd_source);
        return 1;
    }
    uint64_t static_input_end64 = (rdpath && !rd_low) ? rd_range.end
                                                       : ba_range.end;
    if (!align_u64(static_input_end64, 0x4000u, &planned_static_end64) ||
        planned_static_end64 > dram_range.end ||
        (want_fb && planned_static_end64 > fb_range.begin)) {
        fprintf(stderr,
                "layout: static boot reserve ending at 0x%llx reaches the "
                "framebuffer or leaves DRAM\n",
                (unsigned long long)planned_static_end64);
        streamed_file_close(&rd_source);
        return 1;
    }

    /* The complete conservative layout is proved disjoint before this direct
     * write. Keep the source handle open through the final metadata check. */
    if (rdpath) {
        rd = mach.ram + (size_t)(rd_range.begin - dram_range.begin);
        bool read_ok = stream_file(&rd_source, rd, rd_n);
        bool close_ok = streamed_file_close(&rd_source);
        if (!read_ok || !close_ok) {
            fprintf(stderr, "ramdisk: cannot stream %s into guest DRAM\n", rdpath);
            return 1;
        }

        /* Retarget /private/etc/fstab at the device this machine provides.
         * Only guest DRAM is touched; firmware/rootfs.img stays pristine. */
        if (fstab_fixup) {
            size_t at = rd_rewrite_fstab(rd, rd_n, fstab_line);
            if (at == (size_t)-1) return 1;
            printf("fstab      : /private/etc/fstab @ image+0x%08x -> \"%s\"\n"
                   "             (stock names /dev/disk0s1 + /dev/disk0s2; "
                   "this VM has no NAND-backed disk0.\n"
                   "              --keep-fstab leaves it unchanged; launchd "
                   "then fails fsck and halts.)\n",
                   (unsigned)at, fstab_line);
        }

        if (rd_grow_mb && !rd_grow_volume(rd, &rd_n, capacity, rd_grow_mb)) {
            fprintf(stderr, "             pass --grow 0 to boot the volume "
                            "untouched (it has zero free blocks, so launchd "
                            "can create nothing).\n");
            return 1;
        }
    }

    uint64_t rd_len64, rd_actual_end64;
    if (!align_u64((uint64_t)rd_n, 0x1000u, &rd_len64) ||
        rd_len64 > UINT32_MAX ||
        !add_u64(rd_pa64, rd_len64, &rd_actual_end64) ||
        (rdpath && rd_actual_end64 > rd_range.end)) {
        fprintf(stderr, "layout: final padded RAM-disk span exceeds its reserve\n");
        return 1;
    }
    uint32_t rd_len = (uint32_t)rd_len64;

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
    /*
     * MEASURED, and the thing that cost the most time here: topOfKernelData
     * must be 16 KB aligned, not merely page aligned.
     *
     * The kernel derives its L1 translation-table base from this value, and an
     * ARMv6 L1 table is 16 KB aligned by architecture — TTBR0[31:14] is the
     * base and the low bits are attributes. Hand it a value that is only 4 KB
     * aligned and the kernel writes its table at (say) 0x093e5000, TTBR0 comes
     * out 0x093e5018, and the hardware walks from 0x093e4000 instead. The
     * symptom is not a complaint; it is a prefetch abort on the instruction
     * immediately after the MMU is switched on:
     *
     *   FIRST exception entry at instruction 39767: mode 13 -> 17,
     *     caused by pc 0x080691b0, vectored to 0xffff000c  (IFSR 0x05)
     *
     * With no RAM disk the sum happened to land 16 KB aligned, which is why
     * this never showed up before — and why the framebuffer regression noted
     * above looked like a framebuffer problem.
     */
    uint64_t top_of_kernel_data64;
    uint64_t actual_static_end64 = (rd && !rd_low) ? rd_actual_end64
                                                    : ba_range.end;
    if (!align_u64(actual_static_end64, 0x4000u, &top_of_kernel_data64) ||
        top_of_kernel_data64 > planned_static_end64 ||
        top_of_kernel_data64 > UINT32_MAX) {
        fprintf(stderr, "layout: final topOfKernelData exceeds its checked reserve\n");
        return 1;
    }
    uint32_t top_of_kernel_data = (uint32_t)top_of_kernel_data64;
    /* With -Y the disk is below the kernel, so the only thing that can go
     * wrong is running into it. Say so in those terms rather than as a
     * generic "does not fit". */
    if (rd && rd_low) {
        uint32_t kern_pa = (uint32_t)kernel_range.begin;
        if (rd_actual_end64 > kernel_range.begin) {
            fprintf(stderr,
                "-Y: a %u MB RAM disk at 0x%08x overruns the kernel's load "
                "address 0x%08x.\n"
                "    The gap below the kernel is (kernel vmaddr 0x%08x - "
                "virt base 0x%08x) = %u MB;\n"
                "    lower -V to widen it.\n",
                rd_len >> 20, rd_pa, kern_pa, m.vm_low, virt_base,
                (m.vm_low - virt_base) >> 20);
            return 1;
        }
    }
    const uint64_t dram_end64 = dram_range.end;
    uint32_t fb_pa = top_of_kernel_data;

    /* Do all placement arithmetic in 64 bits. A wrapped subtraction here used
     * to turn a too-small RAM aperture into a plausible high physical address,
     * and the final framebuffer scan then indexed outside mach.ram. */
    if ((uint64_t)top_of_kernel_data < phys_base ||
        (uint64_t)top_of_kernel_data > dram_end64) {
        fprintf(stderr,
                "layout: topOfKernelData 0x%08x is outside DRAM "
                "[0x%08x,0x%llx) — reduce the RAM disk or raise -R\n",
                top_of_kernel_data, phys_base,
                (unsigned long long)dram_end64);
        return 1;
    }
    if (want_fb) {
        if ((uint64_t)ram_size < fb_bytes) {
            fprintf(stderr,
                    "framebuffer: %u bytes do not fit in %u bytes of DRAM\n",
                    fb_bytes, ram_size);
            return 1;
        }
        uint64_t start = (dram_end64 - fb_bytes) & ~UINT64_C(0xfff);
        uint64_t end = start + fb_bytes;
        if (start < phys_base || start > UINT32_MAX || end > dram_end64 ||
            (uint64_t)top_of_kernel_data > start) {
            fprintf(stderr,
                    "framebuffer: 0x%llx..0x%llx overlaps the static boot "
                    "layout ending at 0x%08x or lies outside DRAM — "
                    "reduce the RAM disk or raise -R\n",
                    (unsigned long long)start, (unsigned long long)end,
                    top_of_kernel_data);
            return 1;
        }
        fb_pa = (uint32_t)start;
    }

    /*
     * The address to publish in /chosen/memory-map:RAMDisk.
     *
     * SETTLED BY EXPERIMENT, not by reading source (no ARM XNU of this vintage
     * is public). Three facts, each independently checkable:
     *
     *  1. _IOFindBSDRoot at 0xc01a1b5a..0xc01a1b6a is, verbatim,
     *         movs r3,#0 ; ldr r1,[r0] ; ldr r2,[r0,#4]
     *         movs r0,#1 ; lsrs r1,#12 ; lsrs r2,#12 ; rsbs r0,r0,#0
     *         blx  _mdevadd
     *     i.e. mdevadd(-1, parms[0]>>12, parms[1]>>12, 0). No translation, and
     *     _ml_static_ptovirt does not appear anywhere in that function's
     *     literal pool.
     *  2. _mdevadd never touches _gPhysBase / _gVirtBase either; it stores the
     *     page number straight into mdev[].mdBase.
     *  3. phys==0 means mdevstrategy takes the non-mdPhys branch and does a
     *     plain bcopy() from (mdBase << 12) — a kernel VIRTUAL dereference.
     *
     * So the value has to arrive already virtual. That the surrounding code
     * calls ml_static_ptovirt on OTHER memory-map entries (IODTFreeLoaderInfo
     * does, on its way to free them) is what makes this worth stating out loud:
     * memory-map entries are physical BY CONVENTION, and RAMDisk is the one
     * that is not, because nothing on its path ever converts it.
     *
     * -X re-runs the experiment: 'phys' plants the physical address, a literal
     * plants a sentinel, and the _mdevadd watchpoint below prints what actually
     * arrived in r1.
     */
    uint64_t rd_virt64 = 0, dt_virt64 = 0, top_virt64;
    if ((rd &&
         (!add_u64(virt_base, rd_pa64 - dram_range.begin, &rd_virt64) ||
          rd_virt64 > UINT32_MAX)) ||
        (dt &&
         (!add_u64(virt_base, dt_pa64 - dram_range.begin, &dt_virt64) ||
          dt_virt64 > UINT32_MAX)) ||
        !add_u64(virt_base, top_of_kernel_data64 - dram_range.begin,
                 &top_virt64) || top_virt64 > UINT32_MAX) {
        fprintf(stderr, "layout: a boot-data virtual address exceeds 32 bits\n");
        return 1;
    }
    uint32_t rd_dt_addr = rd_form == RD_ADDR_PHYS    ? rd_pa
                        : rd_form == RD_ADDR_LITERAL ? rd_literal
                        : (uint32_t)rd_virt64;
    uint32_t dt_boot_addr = dt ? (uint32_t)dt_virt64 : 0;
    uint32_t top_virt = (uint32_t)top_virt64;

    if (dt) {
        printf("devicetree : %s -> pa 0x%08x (%u bytes)\n", dtpath, dt_pa, dt_len);
        /*
         * Stand in for iBoot: the shipped tree is a template whose clock
         * properties are all zero. Patch the in-memory copy only.
         */
        printf("  --- device-tree patches (iBoot would have done these) ---\n");
        for (unsigned i = 0; i < NDTPATCH; i++)
            dt_set_u32(dt, dt_n, DT_PATCH[i].path, DT_PATCH[i].prop, DT_PATCH[i].val);
        /* The IPSW tree carries zero here because iBoot replaces it with the
         * three-byte ID read from the Syrah panel. Merlot rejects zero before
         * it reaches the target-specific calibration path. */
        if (want_fb &&
            !dt_set_u32(dt, dt_n, "arm-io/spi0/lcd0", "lcd-panel-id",
                        N82_LCD_PANEL_ID)) {
            fprintf(stderr,
                    "framebuffer: cannot patch the N82 lcd-panel-id; "
                    "refusing a half-configured display\n");
            return 1;
        }
        /* /memory reg: the real iBoot fills in the DRAM bank. Ours is
         * zero, which would advertise a zero-sized bank. */
        if (patch_memnode)
            dt_set_reg(dt, dt_n, "memory", "reg", phys_base, ram_size);
        /* NOT touched: the shipped "DeviceTree" entry, whose address is still
         * zero. IODTFreeLoaderInfo() runs ml_static_ptovirt() over that value
         * and then ml_static_mfree()s the result, so filling it in changes what
         * gets freed during early IOKit start. The boot currently gets to
         * _bsd_init with it at zero; correcting it is a separate experiment,
         * not something to smuggle into this one. */
        if (rd)
            dt_memmap_add(dt, dt_n, "RAMDisk", rd_dt_addr, rd_len);
        /* Keep the PowerVR MBX driver from matching unless -g asks for it: it
         * busy-polls a reset bit in a register block we do not model and hangs
         * the boot. Disabled by default so the boot can reach the root device. */
        if (!want_mbx)
            dt_unmatch(dt, dt_n, "arm-io/mbx");
        /* Keep IOCryptoAcceleratorFamily off the SHA-1 nub at 0x38000000.
         *
         * That kext calls sha1_hardware_hook() to install a function pointer in
         * the kernel's _performSHA1WithinKernelOnly. Once it is non-NULL,
         * SHA1UpdateUsePhysicalAddress routes any exactly-4096-byte buffer to
         * the hardware engine instead of running SHA1Transform — and
         * cs_validate_page hashes exactly 4096 bytes. We do not model that
         * register file, so the digest came back fabricated and launchd's first
         * text page failed its signature, spinning on cs_invalid_page forever.
         *
         * The bytes were never wrong: 155 signed Mach-Os and 6731 code pages on
         * the volume all hash correctly, and the timing proves software SHA-1
         * never ran (14,329 instructions observed against ~145,000 needed for
         * 2262 Thumb instructions per 64-byte block).
         *
         * Un-matching is preferable to patching the kernel, and leaves the AES
         * block alone. Model the engine properly if anything ever needs
         * /dev/sha1 or FairPlay. */
        if (!want_sha1hw)
            dt_unmatch(dt, dt_n, "arm-io/sha1");
        for (unsigned i = 0; i < ndtov; i++)
            dt_set_u32(dt, dt_n, dtov[i].path, dtov[i].prop, dtov[i].val);
        printf("  ---------------------------------------------------------\n");
        s5l8900_load(&mach, dt_pa, dt, dt_n);
        free(dt);
    } else if (want_fb) {
        fprintf(stderr,
                "framebuffer: no device tree was supplied; Boot_Video and "
                "CLCD will work, but AppleMerlotLCD cannot be configured\n");
    }

    if (rd) {
        printf("ramdisk    : %s -> pa 0x%08x  %u bytes (%u padded, direct stream)\n",
               rdpath, rd_pa, (unsigned)rd_n, rd_len);
        printf("             DT RAMDisk = {0x%08x, 0x%08x}  (%s)\n",
               rd_dt_addr, rd_len,
               rd_form == RD_ADDR_PHYS ? "physical" :
               rd_form == RD_ADDR_LITERAL ? "literal sentinel" :
               "virtual, ml_static_ptovirt form");
    }

    /*
     * Root-device selector. IOFindBSDRoot compares rdBootVar[0..1] against
     * "md" and rdBootVar[3] against NUL, so the token has to be exactly
     * "md<digit>" — mdevlookup then resolves the minor number.
     */
    char cmdbuf[512];
    if (rd && !strstr(cmdline, "rd=")) {
        snprintf(cmdbuf, sizeof cmdbuf, "%s%srd=md0",
                 cmdline, *cmdline ? " " : "");
        cmdline = cmdbuf;
    }

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
    /*
     * Boot_Video. v_baseAddr must point at a real buffer so _initialize_screen
     * has somewhere to draw; the geometry is always sane (some early code
     * divides by it) even when no framebuffer is advertised.
     *
     * v_display is subtle and cost a while to get right. It is NOT "is there a
     * display" — it selects the console MODE. _PE_create_console branches on
     * it: non-zero picks kPEGraphicsMode, which sets gc_graphics_boot, and then
     * _vcattach returns immediately when gc_graphics_boot is set, so the kernel
     * text console is never acquired and nothing is ever drawn. Zero picks
     * kPETextMode, and the kernel paints its boot log into the framebuffer.
     * So for the kernel to draw, v_display must be 0. It goes back to 1 only
     * once IOMobileFramebuffer takes over scanout. Left as a flag (-g) rather
     * than a constant because both values are correct at different times.
     *
     * Note serial=1 is the second gate: it routes cnputc to the UART instead
     * of the video console, so a graphics boot log also needs serial dropped.
     */
    PUT32(0x14, want_fb ? fb_pa : 0u); /* v_baseAddr */
    PUT32(0x18, want_fb ? v_display : 0u); /* v_display: mode select, see above */
    PUT32(0x1c, FB_W * FB_BPP);        /* v_rowBytes */
    PUT32(0x20, FB_W);                 /* v_width    */
    PUT32(0x24, FB_H);                 /* v_height   */
    PUT32(0x28, FB_BPP * 8);           /* v_depth    */
    PUT32(0x2c, 0);                    /* machineType           */
    PUT32(0x30, dt_boot_addr);
    PUT32(0x34, dt_len);
    memcpy(ba + 0x38, cmdline, strlen(cmdline) < 255 ? strlen(cmdline) : 255);
#undef PUT16
#undef PUT32
    s5l8900_load(&mach, ba_pa, ba, sizeof ba);
    if (want_fb) {
        if (!s5l_clcd_seed_window0(&mach.clcd, fb_pa, FB_W, FB_H,
                                   FB_W * FB_BPP,
                                   CLCD_FMT_32BPP, CLCD_ORDER_BGRA)) {
            fprintf(stderr,
                    "framebuffer: validated layout was rejected by CLCD seed\n");
            return 1;
        }
        printf("framebuffer: pa 0x%08x  %ux%u %u-bit; CLCD window 0 seeded\n",
               fb_pa, FB_W, FB_H, FB_BPP * 8u);
    } else {
        printf("framebuffer: disabled (-F enables Boot_Video + CLCD window 0)\n");
    }
    printf("boot_args  : pa 0x%08x  topOfKernelData vm 0x%08x  cmdline \"%s\"\n\n",
           ba_pa, top_virt, cmdline);

    /*
     * GUEST MEMORY BUDGET — the arithmetic that decides whether a root
     * filesystem this large can be a RAM disk at all.
     *
     * topOfKernelData is not decoration: everything below it is memory the
     * kernel treats as its own pre-loaded static data and NEVER hands to the
     * VM. So the free page pool the whole system runs out of — every process,
     * every page table, the entire buffer cache and every page of any mapped
     * file — is exactly [topOfKernelData, end of DRAM). A RAM disk does not
     * "use up half of RAM"; it moves this line, and what is left is the real
     * budget. Printed unconditionally because it is the number that answers
     * "will it fit", and answering it by hand from three other lines of this
     * header is how the wrong answer gets believed.
     */
    {
        uint32_t dram_end = phys_base + ram_size;
        uint32_t kern_static = top_of_kernel_data - phys_base;
        uint32_t freepool = dram_end - top_of_kernel_data;
        printf("=== GUEST MEMORY BUDGET ===\n");
        printf("  DRAM                 : 0x%08x..0x%08x  %u MB\n",
               phys_base, dram_end, ram_size >> 20);
        uint32_t kern_pa = m.vm_low - virt_base + phys_base;
        uint32_t kern_bytes = (rd && rd_low) ? top_of_kernel_data - kern_pa
                                             : rd_pa - phys_base;
        printf("  kernel image+DT+args : %u.%02u MB\n",
               kern_bytes >> 20, ((kern_bytes & 0xfffffu) * 100u) >> 20);
        printf("  RAM disk             : %u.%02u MB%s\n",
               rd_len >> 20, ((rd_len & 0xfffffu) * 100u) >> 20,
               rd ? (rd_low ? "  (-Y: at the bottom, below the kernel)" : "")
                  : "  (none)");
        if (rd && rd_low) {
            uint32_t gap = kern_pa - (rd_pa + rd_len);
            printf("  unused gap           : %u.%02u MB   [-V puts the kernel "
                   "at 0x%08x; lower it to reclaim this]\n",
                   gap >> 20, ((gap & 0xfffffu) * 100u) >> 20, kern_pa);
        }
        printf("  static (below TOKD)  : %u.%02u MB   [never given to the VM]\n",
               kern_static >> 20, ((kern_static & 0xfffffu) * 100u) >> 20);
        printf("  FREE PAGE POOL       : %u.%02u MB   [everything else lives here]\n",
               freepool >> 20, ((freepool & 0xfffffu) * 100u) >> 20);
        printf("  for scale, a real iPhone1,2 has 128 MB total with the system "
               "on NAND\n");
        /* This deliberately fictional DRAM fit covers real but currently
         * unmodelled eDRAM/VROM/SRAM regions from the device tree. It does not
         * shadow a decoded device: s5l8900_init rejects every overlap with a
         * modelled window, and [0x08000000,0x28000000) ends exactly where the
         * emulator's NOR begins. */
        if (dram_end > S5L8900_EDRAM_BASE)
            printf("  NOTE: this enlarged DRAM window covers unmodelled SoC "
                   "regions\n"
                   "        (eDRAM/VROM/SRAM); no decoded device window is "
                   "shadowed\n");
        printf("\n");
    }

    mach.cpu.r[15] = entry_pa;
    mach.cpu.r[0]  = ba_pa;            /* XNU takes boot_args in r0 */

    /* Interpose on the bus so every non-RAM access is attributed to a PC. */
    spy_install(&mach, virt_base, phys_base);

    /*
     * --restore: replace everything built above with a saved machine.
     *
     * ORDER MATTERS, and it is the reason this is not up next to the kernel
     * load. spy_install() memsets its own diagnostics and captures the CURRENT
     * bus vtable as G.inner before overwriting mach.bus with the spy callbacks;
     * restoring first and installing the spy afterwards would work, but doing
     * it in this order also proves the property snapshot.h advertises — the
     * load preserves host-owned pointers (the RAM allocation, the bus vtable,
     * cpu->bus) and overwrites only their CONTENTS, so the spy stays interposed
     * across a restore and the device-page/UART attribution keeps working.
     *
     * Everything the setup above did to guest RAM (kernel segments, the kpatch,
     * the patched device tree, the RAM disk, boot_args) is simply overwritten
     * by the snapshot's RAM image — which is correct, because the snapshot was
     * taken from a machine that already had all of it. What the setup is still
     * needed for is GEOMETRY: snapshot_load refuses with SNAP_ERR_GEOMETRY
     * unless s5l8900_init() was called with the same ram_base/ram_size, so a
     * restored run must repeat the same -R (and, since -r changes nothing about
     * geometry but everything about what a mismatched log would mean, the same
     * -d/-r/-c too).
     */
    if (restore_path) {
        snapshot_status_t rs = snapshot_load(&mach, restore_path);
        if (rs != SNAP_OK) {
            fprintf(stderr, "restore %s: %s\n", restore_path, snapshot_strerror(rs));
            return 3;
        }
        printf("restored   : %s at %llu retired instructions  (pc 0x%08x)\n\n",
               restore_path, (unsigned long long)mach.cpu.cycles, mach.cpu.r[15]);
        if (want_fb && s5l_clcd_active_window(&mach.clcd) == CLCD_WIN_NONE) {
            fprintf(stderr,
                    "restore: -F was requested but the snapshot has no active "
                    "CLCD window; recreate it from a boot started with -F\n");
            return 3;
        }
    }

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
        /*
         * The root-device path. _mdevadd is the one that settles the
         * physical-vs-virtual question: whatever the device tree said arrives
         * in r1 as a PAGE NUMBER, and if anything between the property and
         * here had translated it, r1 would differ from (planted addr >> 12).
         */
        { "_IOFindBSDRoot", ksym_value("_IOFindBSDRoot"), 0 },
        { "_mdevadd",       ksym_value("_mdevadd"),       0 },
        { "_mdevlookup",    ksym_value("_mdevlookup"),    0 },
    };
    const unsigned nwps = (unsigned)(sizeof wps / sizeof wps[0]);
    for (unsigned i = 0; i < nwps; i++)
        printf("watch      : %-12s vm 0x%08x\n", wps[i].name, wps[i].va);
    vm_resolve();
    printf("\n");

    arm_status_t st = ARM_OK;
    /*
     * ABSOLUTE, not relative. n is an instruction INDEX, and every diagnostic
     * that records one — the milestone "first @ instr", the sampled profile's
     * every-1024 phase, the hot-page time buckets, the abort/FIQ/STREX logs —
     * only means anything if the index a restored run prints is the index the
     * original run would have printed for the same instruction. mach.cpu.cycles
     * is the machine's own retired-instruction count and IS part of the
     * snapshot, so starting from it makes the numbering identical. On a fresh
     * boot it is zero and this is the old `n = 0`.
     */
    unsigned n = (unsigned)mach.cpu.cycles;
    uint32_t last_pc = restore_path ? mach.cpu.r[15] : entry_pa;
    unsigned first_abort_at = 0, first_exc = 0;
    uint32_t abort_dfar = 0, abort_dfsr = 0;

    G.hot_steps = steps;
    for (; n < steps; n++) {
        G.hot_now = n;
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
            for (unsigned i = 0; i < NM; i++) {
                if (p != (MILE[i].va & ~1u) && p != (G.mile_pa[i] & ~1u)) continue;
                if (!G.mile_hits[i]) G.mile_first[i] = n;
                G.mile_hits[i]++;
                break;
            }
        }

        /*
         * What mode is the guest in, and — when it is user mode — where.
         *
         * Cheap enough to do on every instruction, and it has to be: the whole
         * question "is launchd running at all" is a count of instructions whose
         * CPSR mode is 0x10, and a sampled version of that answers "probably
         * none" when the truthful answer is "exactly none".
         */
        {
            unsigned md = mach.cpu.cpsr & ARM_CPSR_MODE_MASK;
            bool thumb = (mach.cpu.cpsr & ARM_CPSR_T) != 0;
            G.mode_all[md]++;
            if (thumb) G.thumb_all++;
            if (n >= win_lo && n < win_hi) {
                G.win_instrs++;
                G.mode_win[md]++;
                if (thumb) G.thumb_win++;
            }
            if (md == ARM_MODE_USR) upc_sample(last_pc);
        }

        /*
         * SYSTEM CALLS. Caught at _fleh_swi, before it has touched anything:
         * r12 is the trap number and r0..r3 the arguments, still the user's
         * because neither is banked in SVC. lr_svc is the return address, so
         * the user PC that issued the SWI is lr-4 (ARM) or lr-2 (Thumb), and
         * SPSR.T says which.
         */
        if (fleh_swi_va &&
            ((last_pc & ~1u) == fleh_swi_va || (last_pc & ~1u) == fleh_swi_pa)) {
            if (G.sc_n < 512) {
                unsigned k = G.sc_n++;
                G.sc[k].at   = n;
                G.sc[k].r12  = mach.cpu.r[12];
                for (unsigned q = 0; q < 4; q++) G.sc[k].r[q] = mach.cpu.r[q];
                G.sc[k].lr   = mach.cpu.r[14];
                G.sc[k].spsr = mach.cpu.spsr[ARM_BANK_SVC];
            } else G.sc_dropped++;
        }

        /* -Z: a heartbeat, so a run that wedges says where without waiting for
         * the final report. */
        if (heartbeat && n && (n % heartbeat) == 0) {
            uint32_t va = last_pc >= phys_base && last_pc < virt_base
                        ? last_pc - phys_base + virt_base : last_pc;
            printf("  [@%-11u] pc %08x m%02x%s  %s\n", n, last_pc,
                   mach.cpu.cpsr & ARM_CPSR_MODE_MASK,
                   (mach.cpu.cpsr & ARM_CPSR_T) ? "T" : " ", ksym_at(va));
            fflush(stdout);
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

        if ((n & 0x3ffu) == 0) {
            uint32_t va = last_pc >= phys_base && last_pc < virt_base
                        ? last_pc - phys_base + virt_base : last_pc;
            /*
             * -W gates the PROFILE ONLY, and that is the whole point of it. A
             * whole-run profile of a boot that stalls is dominated by the work
             * that DID happen — here, ~230M instructions of IOKit driver
             * matching — and so describes the boot rather than the stall. Ask
             * for a window that starts well after the last milestone and the
             * same machinery characterises what is actually spinning.
             */
            if (n >= win_lo && n < win_hi) prof_sample(va);
            if ((n & 0xffffu) == 0) vm_sample(n, steps);
        }

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

        /*
         * FIQ rate and what the handler does to the timer latch. A decrementer
         * of ~60,000 ticks that re-enters every ~100 instructions means the
         * acknowledge is not clearing our latch, and the handler is simply
         * re-entering forever.
         */
        {
            uint32_t mode_after = mach.cpu.cpsr & ARM_CPSR_MODE_MASK;
            if (mode_after == ARM_MODE_FIQ) G.fiq_instrs++;
            if (mode_after == ARM_MODE_FIQ && mode_before != ARM_MODE_FIQ) {
                /* Log the first few, and then the first few *close-spaced*
                 * ones: a storm that starts late is invisible if only the
                 * opening FIQs are sampled. */
                if (G.fiq_n < 12 ||
                    (n - G.fiq_last < 1000 && G.fiq_storm_logged++ < 10))
                    printf("  FIQ #%u @instr %u  gap %u  latch=%08x "
                           "t4_count=%u t4_value=%u ticks=%llu\n",
                           G.fiq_n, n, n - G.fiq_last, mach.timer.irqlatch,
                           mach.timer.t4_count, mach.timer.t4_value,
                           (unsigned long long)mach.timer.ticks);
                G.fiq_last = n;
                G.fiq_n++;
            }
            if (mode_before == ARM_MODE_FIQ && mode_after != ARM_MODE_FIQ) {
                uint64_t dur = (uint64_t)n - G.fiq_last;
                if (dur > G.fiq_longest) G.fiq_longest = dur;
                if (G.fiq_n <= 12)
                    printf("      FIQ exit @instr %u  latch=%08x t4_value=%u\n",
                           n, mach.timer.irqlatch, mach.timer.t4_value);
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
            if (mode_after == ARM_MODE_IRQ && mode_before != ARM_MODE_IRQ)
                G.irq_n++;
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

        /*
         * --snapshot-at: checkpoints, taken last in the body so the machine is
         * exactly one retired instruction past where it was at the top.
         *
         * Keyed on mach.cpu.cycles and NOT on n. The two are equal here, but
         * only cycles is snapshotted: it is the machine's own counter, so a
         * checkpoint requested for instruction 300,000,000 fires at the same
         * instruction whether the process started at 0 or restored at
         * 200,000,000. Keying on a host-side loop variable would make the
         * trigger relative to the process, which is precisely the property
         * that would make two snapshots incomparable.
         */
        for (unsigned s = 0; s < nsnaps; s++) {
            if (snaps[s].at != mach.cpu.cycles) continue;
            snapshot_status_t ss = snapshot_save(&mach, snaps[s].path);
            printf("snapshot   : @%llu -> %s: %s\n",
                   (unsigned long long)mach.cpu.cycles, snaps[s].path,
                   snapshot_strerror(ss));
            fflush(stdout);
            if (ss != SNAP_OK) {
                fprintf(stderr, "snapshot %s: %s\n",
                        snaps[s].path, snapshot_strerror(ss));
                return 4;
            }
        }
    }

    /*
     * The trace ring holds the LAST tcount instructions of the run, not the
     * ones around the first abort — unless -a stopped the run there. Printing
     * it under the heading "instructions leading up to it" and numbering the
     * lines from first_abort_at therefore invented an instruction index for
     * every line: a tail from ~234 M was labelled ~118 M and read as a much
     * earlier fault. Say so instead, and point at the (correctly numbered)
     * end-of-run tail above.
     */
    if (first_abort_at && !stop_on_abort) {
        printf("FIRST data abort at instruction %u: DFSR 0x%08x  DFAR 0x%08x\n",
               first_abort_at, abort_dfsr, abort_dfar);
        printf("  (no trace: the ring holds the END of the run, not this point."
               " Re-run with -a to stop here.)\n\n");
    } else if (first_abort_at) {
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
        uint8_t fetched[4];
        uint32_t fetch_pa = 0;
        const uint8_t *ip = guest_ptr(p, 4);
        bool translated = false;
        if (!ip && guest_fetch_bytes(p, fetched, sizeof fetched, &fetch_pa)) {
            ip = fetched;
            translated = true;
        }
        printf("\n=== ABNORMAL STOP: %s ===\n", status_name(st));
        if (ip) {
            if (mach.cpu.cpsr & ARM_CPSR_T)
                printf("  encoding at pc: %04x %04x (Thumb)\n",
                       (unsigned)(ip[0] | (ip[1] << 8)),
                       (unsigned)(ip[2] | (ip[3] << 8)));
            else
                printf("  encoding at pc: 0x%08x (ARM)\n", ld32(ip));
            if (translated)
                printf("  fetch mapping : va 0x%08x -> pa 0x%08x\n", p, fetch_pa);
        } else {
            printf("  encoding at pc: unavailable (fetch translation failed)\n");
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
    for (unsigned v = 0; v < S5L8900_VIC_COUNT; v++)
        printf("  VIC%u           : raw=%08x soft=%08x en=%08x sel=%08x  irq=%d fiq=%d\n",
               v, mach.vic[v].raw, mach.vic[v].soft, mach.vic[v].enable,
               mach.vic[v].select,
               s5l_vic_irq(&mach.vic[v]), s5l_vic_fiq(&mach.vic[v]));
    printf("  CLCD           : status=%08x mask=%08x scanning=%d\n",
           mach.clcd.intstatus, mach.clcd.intmask, mach.clcd.scanning);
    printf("  CPU lines      : irq=%d fiq=%d\n", mach.cpu.irq_line, mach.cpu.fiq_line);

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

    /* ------------------------------------------------ the hot MMIO page --- */
    printf("\n=== HOT PAGE 0x%08x: PER-REGISTER ===\n", HOTPG);
    printf("    off    reads      writes     lastval    firstwrite\n");
    for (unsigned i = 0; i < 1024; i++) {
        char fw[16];
        if (!G.hot_r[i] && !G.hot_w[i]) continue;
        if (G.hot_written[i]) snprintf(fw, sizeof fw, "0x%08x", G.hot_firstw[i]);
        else                  snprintf(fw, sizeof fw, "-");
        printf("    0x%03x  %-10llu %-10llu 0x%08x %s\n", i * 4,
               (unsigned long long)G.hot_r[i], (unsigned long long)G.hot_w[i],
               G.hot_last[i], fw);
    }

    printf("\n=== HOT PAGE 0x%08x: ACCESS SITES (pc/lr/off) ===\n", HOTPG);
    for (unsigned i = 0; i < G.hot_site_n; i++)
        printf("    %s off 0x%03x  n=%-10llu pc 0x%08x  lr 0x%08x  instr %llu..%llu\n",
               G.hot_site[i].wr ? "W" : "R", G.hot_site[i].off,
               (unsigned long long)G.hot_site[i].n, G.hot_site[i].pc, G.hot_site[i].lr,
               (unsigned long long)G.hot_site[i].first_at,
               (unsigned long long)G.hot_site[i].last_at);

    printf("\n=== HOT PAGE 0x%08x: FIRST %u ACCESSES ===\n", HOTPG, G.hot_log_n);
    for (unsigned i = 0; i < G.hot_log_n; i++)
        printf("    @%-10llu %s%u 0x%08x (off 0x%03x) val 0x%08x  pc 0x%08x lr 0x%08x"
               "  r0-5 %08x %08x %08x %08x %08x %08x\n",
               (unsigned long long)G.hot_log[i].at,
               G.hot_log[i].wr ? "W" : "R", G.hot_log[i].bytes * 8,
               G.hot_log[i].addr, G.hot_log[i].addr & 0xfff, G.hot_log[i].val,
               G.hot_log[i].pc, G.hot_log[i].lr,
               G.hot_log[i].r[0], G.hot_log[i].r[1], G.hot_log[i].r[2],
               G.hot_log[i].r[3], G.hot_log[i].r[4], G.hot_log[i].r[5]);

    printf("\n=== HOT PAGE 0x%08x: LAST ACCESSES (of %llu) ===\n", HOTPG,
           (unsigned long long)G.hot_tail_n);
    {
        unsigned cnt = G.hot_tail_n < 64 ? (unsigned)G.hot_tail_n : 64;
        unsigned start = (G.hot_tail_w + 64 - cnt) % 64;
        for (unsigned i = 0; i < cnt; i++) {
            unsigned k = (start + i) % 64;
            printf("    @%-10llu %s 0x%08x (off 0x%03x) val 0x%08x  pc 0x%08x lr 0x%08x\n",
                   (unsigned long long)G.hot_tail[k].at, G.hot_tail[k].wr ? "W" : "R",
                   G.hot_tail[k].addr, G.hot_tail[k].addr & 0xfff, G.hot_tail[k].val,
                   G.hot_tail[k].pc, G.hot_tail[k].lr);
        }
    }

    printf("\n=== HOT PAGE 0x%08x: ACCESSES OVER TIME (40 buckets) ===\n", HOTPG);
    for (unsigned i = 0; i < 40; i++)
        if (G.hot_bucket[i])
            printf("    instr %10llu..%-10llu  %llu\n",
                   (unsigned long long)(steps / 40 * i),
                   (unsigned long long)(steps / 40 * (i + 1)),
                   (unsigned long long)G.hot_bucket[i]);

    /*
     * WHERE THE RUN ENDED, instruction by instruction.
     *
     * The abort dump below already prints this ring, but only when there was
     * an abort. A run that simply hits its step limit while spinning printed
     * nothing at all — and the last few hundred instructions of a spin ARE the
     * spin. Two views: the distinct functions on the way in (the call path),
     * and the raw tail (the loop body).
     */
    {
        unsigned ntail = ktail < 200 ? ktail : 200;
        printf("\n=== LAST %u INSTRUCTIONS BEFORE THE RUN ENDED ===\n", ntail);
        unsigned start = (tw + KTRACE - tcount) % KTRACE;
        printf("  distinct functions over the last %u instructions:\n",
               tcount > 65536 ? 65536 : tcount);
        {
            char seen[160]; seen[0] = '\0';
            unsigned from = tcount > 65536 ? tcount - 65536 : 0;
            for (unsigned i = from; i < tcount; i++) {
                unsigned k = (start + i) % KTRACE;
                uint32_t va = tr[k].pc >= phys_base && tr[k].pc < virt_base
                            ? tr[k].pc - phys_base + virt_base : tr[k].pc;
                const char *nm = ksym_at(va);
                char base[160];
                const char *bar = strchr(nm, '+');
                snprintf(base, sizeof base, "%.*s",
                         bar ? (int)(bar - nm) : (int)strlen(nm), nm);
                if (strcmp(base, seen)) {
                    printf("    %-9u %08x m%02x  %s\n",
                           n - (tcount - 1 - i), tr[k].pc,
                           tr[k].cpsr & ARM_CPSR_MODE_MASK, nm);
                    snprintf(seen, sizeof seen, "%s", base);
                }
            }
        }
        printf("  raw tail:\n");
        unsigned skip = tcount > ntail ? tcount - ntail : 0;
        for (unsigned i = skip; i < tcount; i++) {
            unsigned k = (start + i) % KTRACE;
            uint32_t va = tr[k].pc >= phys_base && tr[k].pc < virt_base
                        ? tr[k].pc - phys_base + virt_base : tr[k].pc;
            printf("    %-9u %08x %c m%02x r0=%08x r1=%08x r2=%08x r3=%08x "
                   "sp=%08x lr=%08x  %s\n",
                   n - (tcount - 1 - i), tr[k].pc,
                   (tr[k].cpsr & ARM_CPSR_T) ? 'T' : 'A',
                   tr[k].cpsr & ARM_CPSR_MODE_MASK,
                   tr[k].r[0], tr[k].r[1], tr[k].r[2], tr[k].r[3],
                   tr[k].r[13], tr[k].r[14], ksym_at(va));
        }
    }

    printf("\n=== CONSOLE-INIT MILESTONES (xnu-1357.5.30 symbols) ===\n");
    printf("    --- NEVER REACHED ---\n");
    for (unsigned i = 0; i < NM; i++)
        if (!G.mile_hits[i])
            printf("    %-28s vm 0x%08x\n", MILE[i].name, MILE[i].va);
    printf("    --- reached, with call counts and first instruction index ---\n");
    for (unsigned i = 0; i < NM; i++)
        if (G.mile_hits[i])
            printf("    %-28s hits %-10llu first @ instr %u\n", MILE[i].name,
                   (unsigned long long)G.mile_hits[i], G.mile_first[i]);

    mode_report(n, win_lo, win_hi);
    syscall_report(virt_base, phys_base);

    printf("\n=== WHERE THE TIME WENT (sampled every 1024 instructions%s) ===\n",
           (win_lo || win_hi != 0xffffffffu) ? ", WINDOWED" : "");
    if (win_lo || win_hi != 0xffffffffu)
        printf("    window: instructions %u .. %u  (-W)\n", win_lo, win_hi);
    {
        unsigned total = 0;
        for (unsigned i = 0; i < G.prof_n; i++) total += G.prof[i].hits;
        printf("    %u samples over %u distinct functions%s\n", total, G.prof_n,
               G.prof_dropped ? "" : "");
        if (G.prof_dropped)
            printf("    WARNING: %u samples dropped (table full) — profile is "
                   "NOT representative\n", G.prof_dropped);
        for (unsigned rank = 0; rank < 12; rank++) {
            unsigned best = 0, bi = G.prof_n;
            for (unsigned i = 0; i < G.prof_n; i++)
                if (G.prof[i].hits > best) { best = G.prof[i].hits; bi = i; }
            if (bi == G.prof_n || !best) break;
            printf("    %5.1f%%  %-44s %u samples\n",
                   total ? 100.0 * best / total : 0.0, G.prof[bi].fn, best);
            G.prof[bi].hits = 0;   /* consume */
        }
    }

    /*
     * Which prelinked kext the time went to.
     *
     * This is the report that ends the recurring diagnosis cycle. When a
     * driver spins, its samples spread across the handful of PCs in one poll
     * loop, so no single function dominates while the KEXT plainly does —
     * which is exactly what used to read as "66.9% in one unsymbolized kext"
     * and cost a whole cycle to attribute by hand. Tallied before the top-PC
     * list below consumes entries.
     */
    if (KS.nkext_exec) {
        static unsigned per_kext[KSYMS_MAX_KEXTS];
        unsigned total = 0, attributed = 0;
        for (unsigned i = 0; i < PCHASH; i++) {
            if (!G.pc_hist[i].hits) continue;
            total += G.pc_hist[i].hits;
            const kext_t *k = ksyms_kext_at(&KS, G.pc_hist[i].va);
            if (!k) continue;
            per_kext[(unsigned)(k - KS.kext)] += G.pc_hist[i].hits;
            attributed += G.pc_hist[i].hits;
        }
        printf("\n=== TIME BY PRELINKED KEXT ===\n");
        printf("    %.1f%% of samples are inside a prelinked kext; the rest is "
               "the kernel proper\n", total ? 100.0 * attributed / total : 0.0);
        for (unsigned rank = 0; rank < 10; rank++) {
            unsigned best = 0, bi = KS.nkext_exec;
            for (unsigned i = 0; i < KS.nkext_exec; i++)
                if (per_kext[i] > best) { best = per_kext[i]; bi = i; }
            if (bi == KS.nkext_exec || !best) break;
            printf("    %5.1f%%  %-46s 0x%08x+0x%x\n",
                   total ? 100.0 * best / total : 0.0, KS.kext[bi].bundle,
                   KS.kext[bi].addr, KS.kext[bi].size);
            per_kext[bi] = 0;
        }
    }

    /*
     * The same samples by exact PC. A kext carries no symbol table, so this is
     * the finest name available inside one: 0xc0778122 prints as
     * com.apple.driver.AppleMBX+0x122 — an address to disassemble rather than
     * a mystery to bisect.
     */
    printf("\n=== HOTTEST INDIVIDUAL PCs (same samples, exact address) ===\n");
    {
        unsigned total = 0;
        for (unsigned i = 0; i < PCHASH; i++) total += G.pc_hist[i].hits;
        printf("    %u samples over %u distinct PCs\n", total, G.pc_n);
        if (G.pc_dropped)
            printf("    WARNING: %u samples dropped (hash full) — this list is "
                   "NOT complete\n", G.pc_dropped);
        for (unsigned rank = 0; rank < 16; rank++) {
            unsigned best = 0, bi = PCHASH;
            for (unsigned i = 0; i < PCHASH; i++)
                if (G.pc_hist[i].hits > best) { best = G.pc_hist[i].hits; bi = i; }
            if (bi == PCHASH || !best) break;
            printf("    %5.1f%%  0x%08x  %-52s %u samples\n",
                   total ? 100.0 * best / total : 0.0,
                   G.pc_hist[bi].va, ksym_at(G.pc_hist[bi].va), best);
            G.pc_hist[bi].hits = 0;   /* consume */
        }
    }

    printf("\n=== FIQ COST ===\n");
    printf("    entries              : %u\n", G.fiq_n);
    printf("    instructions in FIQ  : %llu (%.1f%% of the run)\n",
           (unsigned long long)G.fiq_instrs,
           100.0 * (double)G.fiq_instrs / (double)(n ? n : 1));
    printf("    longest single FIQ   : %llu instructions\n",
           (unsigned long long)G.fiq_longest);

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
    if (G.fault_dropped)
        printf("    WARNING: %u aborts at NEW sites were dropped (table full)"
               " — this list is NOT complete\n", G.fault_dropped);
    for (unsigned i = 0; i < G.fault_n; i++)
        printf("    %s FAR 0x%08x FSR 0x%02x  pc 0x%08x %s  n=%llu first@%u\n",
               G.fault[i].prefetch ? "IFETCH" : "DATA  ",
               G.fault[i].far_, G.fault[i].fsr & 0xff, G.fault[i].pc,
               ksym_at(G.fault[i].pc >= phys_base && G.fault[i].pc < virt_base
                       ? G.fault[i].pc - phys_base + virt_base : G.fault[i].pc),
               (unsigned long long)G.fault[i].n, G.fault[i].first_at);

    vm_report();

    if (mach.uart0.tx_len)
        printf("\n=== KERNEL UART OUTPUT (%zu bytes) ===\n%s\n",
               mach.uart0.tx_len, mach.uart0.tx);
    else
        printf("\n(no UART output yet)\n");

    /* Did the kernel actually draw? Read the controller's CURRENT active
     * window, not merely the original Boot_Video address: IOMFB is allowed to
     * swap FBADDR after adopting iBoot's surface. The core scanout helper owns
     * all source-range and destination-size checks. */
    if (want_fb) {
        const size_t rgb_n = (size_t)FB_W * FB_H * 3u;
        uint8_t *rgb = calloc(rgb_n, 1);
        unsigned active = s5l_clcd_active_window(&mach.clcd);
        uint32_t out_w = 0, out_h = 0;
        if (!rgb) {
            fprintf(stderr, "framebuffer: cannot allocate %zu-byte RGB capture\n",
                    rgb_n);
        } else if (active == CLCD_WIN_NONE) {
            fprintf(stderr, "framebuffer: CLCD has no active RGB window\n");
        } else if (!s5l_clcd_scanout(&mach.clcd, active,
                                     mach.ram, mach.ram_base, mach.ram_size,
                                     rgb, rgb_n, &out_w, &out_h)) {
            fprintf(stderr,
                    "framebuffer: active CLCD window %u is malformed, outside "
                    "DRAM, or larger than the N82 capture buffer\n", active);
        } else {
            size_t out_n = (size_t)out_w * out_h * 3u;
            size_t nonzero = 0;
            for (size_t i = 0; i < out_n; i++)
                if (rgb[i]) nonzero++;
            printf("\nframebuffer: CLCD window %u, %ux%u, "
                   "%zu of %zu RGB bytes non-zero\n",
                   active, out_w, out_h, nonzero, out_n);
            if (nonzero) {
                FILE *o = fopen("firmware/screen.ppm", "wb");
                if (o) {
                    bool wrote = fprintf(o, "P6\n%u %u\n255\n", out_w, out_h) > 0 &&
                                 fwrite(rgb, 1, out_n, o) == out_n;
                    if (fclose(o) != 0) wrote = false;
                    if (wrote)
                        printf("wrote firmware/screen.ppm - THE KERNEL DREW SOMETHING\n");
                    else
                        fprintf(stderr,
                                "framebuffer: firmware/screen.ppm write failed\n");
                } else {
                    perror("framebuffer: open firmware/screen.ppm");
                }
            }
        }
        free(rgb);
    }

    s5l8900_free(&mach);
    ksyms_free(&KS);
    free(img);
    return 0;
}

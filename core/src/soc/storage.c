/*
 * iOS3-VM — persistent storage backing (see storage.h for why this exists).
 *
 * The container is deliberately simple and self-describing: a magic, a version,
 * and the geometry the image was written with. Restoring into a device of a
 * different shape is refused rather than silently reinterpreted, because a
 * misread NAND image is exactly the kind of failure that looks like a guest
 * bug for days.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "storage.h"
#include <stdio.h>
#include <string.h>

#define STORAGE_MAGIC   0x334f4956u   /* "VIO3" */
#define STORAGE_VERSION 1u
#define HEADER_WORDS    8u            /* magic, version, 4 geometry, 2 reserved */

const char *storage_strerror(storage_status_t st) {
    switch (st) {
        case STORAGE_OK:            return "ok";
        case STORAGE_ERR_IO:        return "could not read or write the file";
        case STORAGE_ERR_FORMAT:    return "not an iOS3-VM storage image";
        case STORAGE_ERR_VERSION:   return "image written by an incompatible version";
        case STORAGE_ERR_GEOMETRY:  return "image geometry does not match the device";
        case STORAGE_ERR_TRUNCATED: return "image is shorter than its header promises";
        default:                    return "unknown error";
    }
}

static bool wr32(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    return fwrite(b, 1, 4, f) == 4;
}
static bool rd32(FILE *f, uint32_t *v) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8)
       | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

storage_status_t storage_save_nand(const nand_t *n, const char *path) {
    if (!n || !n->data || !path) return STORAGE_ERR_IO;

    FILE *f = fopen(path, "wb");
    if (!f) return STORAGE_ERR_IO;

    uint32_t pages = nand_total_pages(n);
    bool ok = wr32(f, STORAGE_MAGIC) && wr32(f, STORAGE_VERSION)
           && wr32(f, n->page_size) && wr32(f, n->spare_size)
           && wr32(f, n->pages_per_block) && wr32(f, n->block_count)
           && wr32(f, 0) && wr32(f, 0);

    if (ok) ok = fwrite(n->data, 1, (size_t)pages * n->page_size, f)
                 == (size_t)pages * n->page_size;
    if (ok && n->spare && n->spare_size)
        ok = fwrite(n->spare, 1, (size_t)pages * n->spare_size, f)
             == (size_t)pages * n->spare_size;
    if (ok) ok = fwrite(n->bad, 1, n->block_count, f) == n->block_count;

    /* Report a write error rather than leaving the caller believing the guest's
     * state is safely on disk. */
    if (fclose(f) != 0) ok = false;
    return ok ? STORAGE_OK : STORAGE_ERR_IO;
}

storage_status_t storage_load_nand(nand_t *n, const char *path) {
    if (!n || !n->data || !path) return STORAGE_ERR_IO;

    FILE *f = fopen(path, "rb");
    if (!f) return STORAGE_ERR_IO;

    uint32_t hdr[HEADER_WORDS];
    for (unsigned i = 0; i < HEADER_WORDS; i++) {
        if (!rd32(f, &hdr[i])) { fclose(f); return STORAGE_ERR_TRUNCATED; }
    }
    if (hdr[0] != STORAGE_MAGIC)   { fclose(f); return STORAGE_ERR_FORMAT; }
    if (hdr[1] != STORAGE_VERSION) { fclose(f); return STORAGE_ERR_VERSION; }

    /* Refuse a geometry mismatch instead of reading the bytes anyway. */
    if (hdr[2] != n->page_size || hdr[3] != n->spare_size ||
        hdr[4] != n->pages_per_block || hdr[5] != n->block_count) {
        fclose(f);
        return STORAGE_ERR_GEOMETRY;
    }

    uint32_t pages = nand_total_pages(n);
    size_t dbytes = (size_t)pages * n->page_size;
    size_t sbytes = (size_t)pages * n->spare_size;

    bool ok = fread(n->data, 1, dbytes, f) == dbytes;
    if (ok && n->spare && n->spare_size) ok = fread(n->spare, 1, sbytes, f) == sbytes;
    if (ok) ok = fread(n->bad, 1, n->block_count, f) == n->block_count;

    fclose(f);
    return ok ? STORAGE_OK : STORAGE_ERR_TRUNCATED;
}

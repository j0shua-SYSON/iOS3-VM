/*
 * iOS3-VM — S5L8900 NOR flash.
 *
 * Holds the low-level boot images. Reads are served memory-mapped; the image
 * directory is built by scanning for IMG3 containers rather than by parsing a
 * guessed-at Apple image table (see soc.h for why).
 *
 * The scanner treats NOR contents as untrusted: every candidate header is
 * bounds-checked against the flash size before it is believed, so a corrupt or
 * hostile dump produces a short directory, never an out-of-bounds read.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <stdlib.h>
#include <string.h>

#define IMG3_DISK_MAGIC 0x33676d49u   /* "3gmI" as stored */
#define IMG3_HEADER     20u

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool s5l_nor_init(s5l_nor_t *n, uint32_t size) {
    memset(n, 0, sizeof *n);
    n->data = calloc(size ? size : 1u, 1);
    if (!n->data) return false;
    n->size = size;
    return true;
}

void s5l_nor_free(s5l_nor_t *n) {
    free(n->data);
    n->data = NULL;
    n->size = 0;
    n->image_count = 0;
}

uint32_t s5l_nor_read(const s5l_nor_t *n, uint32_t off, unsigned bytes) {
    /* 64-bit comparison so an offset near the top of the space cannot wrap. */
    if (!n->data || (uint64_t)off + bytes > (uint64_t)n->size) return 0;
    uint32_t v = 0;
    memcpy(&v, &n->data[off], bytes);
    return v;
}

void s5l_nor_program(s5l_nor_t *n, uint32_t off, const void *src, size_t len) {
    if (!n->data || len > n->size) return;
    if ((uint64_t)off + len > (uint64_t)n->size) return;
    memcpy(&n->data[off], src, len);
}

unsigned s5l_nor_scan(s5l_nor_t *n) {
    n->image_count = 0;
    if (!n->data || n->size < IMG3_HEADER) return 0;

    /* IMG3 containers are word-aligned in flash, so step by 4. */
    uint32_t off = 0;
    while ((uint64_t)off + IMG3_HEADER <= (uint64_t)n->size) {
        if (rd32(&n->data[off]) != IMG3_DISK_MAGIC) { off += 4; continue; }

        uint32_t full  = rd32(&n->data[off + 4]);
        uint32_t ident = rd32(&n->data[off + 16]);

        /* Believe the header only if the container actually fits. */
        if (full < IMG3_HEADER || (uint64_t)off + full > (uint64_t)n->size) {
            off += 4;
            continue;
        }

        n->images[n->image_count].ident  = ident;
        n->images[n->image_count].offset = off;
        n->images[n->image_count].size   = full;
        if (++n->image_count == S5L_NOR_MAX_IMAGES) break;

        /* Skip past the container, then re-align: a container whose size is not
         * a multiple of 4 would otherwise leave the scan permanently off the
         * word grid and it would never see any later image. */
        uint32_t next = (uint32_t)(((uint64_t)off + full + 3u) & ~(uint64_t)3u);
        if (next <= off) break;                  /* never move backwards */
        off = next;
    }
    return n->image_count;
}

const s5l_nor_entry_t *s5l_nor_find(const s5l_nor_t *n, uint32_t ident) {
    for (unsigned i = 0; i < n->image_count; i++)
        if (n->images[i].ident == ident) return &n->images[i];
    return NULL;
}

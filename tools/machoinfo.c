/*
 * iOS3-VM — machoinfo: describe the decompressed XNU kernel.
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "macho.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <kernel.macho>\n", argv[0]); return 1; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); rewind(f);
    uint8_t *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);

    macho_t m;
    macho_status_t st = macho_parse(b, (size_t)n, &m);
    printf("parse      : %s\n", macho_strerror(st));
    if (st != MACHO_OK) return 2;

    printf("cputype    : %u  subtype %u  filetype %u\n", m.cputype, m.cpusubtype, m.filetype);
    printf("segments   : %u\n", m.segment_count);
    for (unsigned i = 0; i < m.segment_count; i++) {
        macho_segment_t *s = &m.segments[i];
        printf("  %-16s vm 0x%08x..0x%08x  file 0x%08x + %u\n",
               s->name, s->vmaddr, s->vmaddr + s->vmsize, s->fileoff, s->filesize);
    }
    printf("vm range   : 0x%08x .. 0x%08x (%.1f MB)\n",
           m.vm_low, m.vm_high, (m.vm_high - m.vm_low) / 1048576.0);
    if (m.has_entry) printf("entry PC   : 0x%08x   (initial SP 0x%08x)\n", m.entry, m.entry_sp);
    else             printf("entry PC   : NOT FOUND (no LC_UNIXTHREAD)\n");
    return 0;
}

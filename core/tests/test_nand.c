/*
 * iOS3-VM — NAND device tests.
 *
 * The important property here is that the model behaves like real NAND rather
 * than like RAM: erased pages read as all ones, programming can only clear
 * bits, and out-of-range or bad-block access is refused. Getting that wrong
 * produces an emulator that works until it meets real firmware.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "nand.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* A small device: 64-byte pages, 8 bytes spare, 4 pages/block, 4 blocks. */
static bool small(nand_t *n) { return nand_init(n, 64, 8, 4, 4); }

static void test_erased_reads_all_ones(void) {
    nand_t n; CHECK(small(&n), "init failed");
    uint8_t buf[64], spare[8];
    CHECK(nand_read_page(&n, 0, buf, spare) == NAND_OK, "read failed");
    bool ones = true;
    for (unsigned i = 0; i < 64; i++) if (buf[i] != 0xff) ones = false;
    for (unsigned i = 0; i < 8; i++)  if (spare[i] != 0xff) ones = false;
    CHECK(ones, "erased NAND should read as all ones");
    nand_free(&n);
}

static void test_program_and_read_back(void) {
    nand_t n; small(&n);
    uint8_t page[64], spare[8], back[64], bspare[8];
    for (unsigned i = 0; i < 64; i++) page[i] = (uint8_t)i;
    for (unsigned i = 0; i < 8; i++)  spare[i] = (uint8_t)(0xa0 + i);

    CHECK(nand_program_page(&n, 3, page, spare) == NAND_OK, "program failed");
    CHECK(nand_read_page(&n, 3, back, bspare) == NAND_OK, "read failed");
    CHECK(memcmp(page, back, 64) == 0, "data did not round-trip");
    CHECK(memcmp(spare, bspare, 8) == 0, "spare did not round-trip");
    nand_free(&n);
}

static void test_program_can_only_clear_bits(void) {
    /* The defining property of NAND. Writing 0xFF over 0x00 must be refused,
     * not silently "work" the way it would on RAM. */
    nand_t n; small(&n);
    uint8_t zeros[64], ones[64], spare[8];
    memset(zeros, 0x00, sizeof zeros);
    memset(ones,  0xff, sizeof ones);
    memset(spare, 0xff, sizeof spare);

    CHECK(nand_program_page(&n, 0, zeros, spare) == NAND_OK, "first program failed");
    CHECK(nand_program_page(&n, 0, ones, spare) == NAND_ERR_NOT_ERASED,
          "programming 1s over 0s should be refused");

    /* Clearing further bits in an already-programmed page is legal. */
    uint8_t half[64];
    memset(half, 0x00, sizeof half);
    CHECK(nand_program_page(&n, 0, half, spare) == NAND_OK,
          "re-clearing already-zero bits should be allowed");
    nand_free(&n);
}

static void test_erase_restores_ones(void) {
    nand_t n; small(&n);
    uint8_t zeros[64], back[64], spare[8];
    memset(zeros, 0x00, sizeof zeros);
    memset(spare, 0x00, sizeof spare);
    nand_program_page(&n, 5, zeros, spare);        /* page 5 is in block 1 */

    CHECK(nand_erase_block(&n, 1) == NAND_OK, "erase failed");
    CHECK(nand_read_page(&n, 5, back, NULL) == NAND_OK, "read failed");
    bool ones = true;
    for (unsigned i = 0; i < 64; i++) if (back[i] != 0xff) ones = false;
    CHECK(ones, "erase should restore all ones");

    /* Erasing block 1 must not have touched block 0. */
    nand_program_page(&n, 0, zeros, NULL);
    nand_erase_block(&n, 1);
    nand_read_page(&n, 0, back, NULL);
    CHECK(back[0] == 0x00, "erasing block 1 disturbed block 0");
    nand_free(&n);
}

static void test_range_and_bad_blocks(void) {
    nand_t n; small(&n);
    uint8_t buf[64];
    CHECK(nand_total_pages(&n) == 16, "total pages=%u expect 16", nand_total_pages(&n));
    CHECK(nand_read_page(&n, 16, buf, NULL) == NAND_ERR_RANGE, "page 16 accepted");
    CHECK(nand_read_page(&n, 0xffffffffu, buf, NULL) == NAND_ERR_RANGE,
          "huge page index accepted");
    CHECK(nand_erase_block(&n, 4) == NAND_ERR_RANGE, "block 4 accepted");

    nand_mark_bad(&n, 2);
    CHECK(nand_is_bad(&n, 2), "block 2 should be bad");
    CHECK(!nand_is_bad(&n, 1), "block 1 wrongly bad");
    memset(buf, 0, sizeof buf);
    CHECK(nand_program_page(&n, 8, buf, NULL) == NAND_ERR_BAD_BLOCK,
          "programming a bad block should be refused");
    CHECK(nand_erase_block(&n, 2) == NAND_ERR_BAD_BLOCK,
          "erasing a bad block should be refused");
    nand_free(&n);
}

static void test_absurd_geometry_refused(void) {
    nand_t n;
    CHECK(!nand_init(&n, 0, 8, 4, 4), "zero page size accepted");
    CHECK(!nand_init(&n, 64, 8, 0, 4), "zero pages-per-block accepted");
    CHECK(!nand_init(&n, 64, 8, 4, 0), "zero block count accepted");
    /* A geometry whose page count overflows 32 bits must be refused. */
    CHECK(!nand_init(&n, 2048, 64, 0x10000u, 0x10000u),
          "overflowing geometry accepted");
}

int main(void) {
    printf("iOS3-VM NAND device tests\n");
    test_erased_reads_all_ones();
    test_program_and_read_back();
    test_program_can_only_clear_bits();
    test_erase_restores_ones();
    test_range_and_bad_blocks();
    test_absurd_geometry_refused();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

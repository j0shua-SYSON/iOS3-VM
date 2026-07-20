/*
 * iOS3-VM — IMG3 parser tests.
 *
 * Builds IMG3 containers in memory (we ship no Apple firmware) and checks both
 * correct parsing and, importantly, that malformed containers are rejected
 * rather than read out of bounds — this parser is the first thing to touch
 * untrusted user-supplied files.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "img3.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* -------------------------------------------------------- image building */
static uint8_t  g_buf[4096];
static uint32_t g_len;

static void put32(uint32_t off, uint32_t v) {
    g_buf[off] = (uint8_t)v; g_buf[off+1] = (uint8_t)(v >> 8);
    g_buf[off+2] = (uint8_t)(v >> 16); g_buf[off+3] = (uint8_t)(v >> 24);
}

/* Start a container with the given ident (e.g. 'ibot' as 0x69626f74). */
static void img_begin(uint32_t ident) {
    memset(g_buf, 0, sizeof g_buf);
    put32(0, 0x33676d49u);      /* "3gmI" on disk */
    put32(16, ident);
    g_len = 20;
}

/* Append a tag with `data_len` bytes of payload copied from `data`. */
static uint32_t img_tag(uint32_t magic, const void *data, uint32_t data_len) {
    uint32_t off = g_len;
    uint32_t total = 12 + data_len;
    put32(off, magic); put32(off + 4, total); put32(off + 8, data_len);
    if (data && data_len) memcpy(&g_buf[off + 12], data, data_len);
    g_len += total;
    return off;
}

static void img_finish(void) {
    put32(4, g_len);            /* fullSize   */
    put32(8, g_len - 20);       /* dataSize   */
    put32(12, g_len - 20);      /* signedSize */
}

/* -------------------------------------------------------------- the tests */

static void test_parse_minimal(void) {
    const char payload[] = "IBOOTPAYLOAD";
    img_begin(0x69626f74u);                       /* 'ibot' */
    img_tag(IMG3_TAG_DATA, payload, (uint32_t)strlen(payload));
    img_finish();

    img3_t img;
    img3_status_t st = img3_parse(g_buf, g_len, &img);
    CHECK(st == IMG3_OK, "status=%s", img3_strerror(st));
    CHECK(img.data != NULL && img.data_len == strlen(payload),
          "data_len=%u expect %u", img.data_len, (unsigned)strlen(payload));
    CHECK(img.data && memcmp(img.data, payload, strlen(payload)) == 0,
          "payload mismatch");

    char id[5]; img3_ident_str(img.ident, id);
    CHECK(strcmp(id, "ibot") == 0, "ident=\"%s\" expect ibot", id);
}

static void test_parse_kbag_and_vers(void) {
    uint8_t kbag[24 + 16];
    memset(kbag, 0, sizeof kbag);
    kbag[0] = 1;                                  /* cryptState = 1 (production) */
    kbag[4] = 128;                                /* keyBits = 128 */
    for (int i = 0; i < 16; i++) kbag[8 + i]  = (uint8_t)(0xa0 + i);   /* IV  */
    for (int i = 0; i < 16; i++) kbag[24 + i] = (uint8_t)(0xb0 + i);   /* key */

    uint8_t vers[4 + 5];
    memset(vers, 0, sizeof vers);
    vers[0] = 5;                                  /* length prefix */
    memcpy(&vers[4], "3.1.3", 5);

    img_begin(0x6b726e6cu);                       /* 'krnl' */
    img_tag(IMG3_TAG_VERS, vers, sizeof vers);
    img_tag(IMG3_TAG_KBAG, kbag, sizeof kbag);
    img_tag(IMG3_TAG_DATA, "xxxx", 4);
    img_tag(IMG3_TAG_SHSH, "sig", 3);
    img_finish();

    img3_t img;
    CHECK(img3_parse(g_buf, g_len, &img) == IMG3_OK, "parse failed");
    CHECK(img.kbag.present, "kbag not parsed");
    CHECK(img.kbag.crypt_state == 1, "crypt_state=%u expect 1", img.kbag.crypt_state);
    CHECK(img.kbag.key_bits == 128, "key_bits=%u expect 128", img.kbag.key_bits);
    CHECK(img.kbag.iv[0] == 0xa0 && img.kbag.iv[15] == 0xaf, "IV mismatch");
    CHECK(img.kbag.key[0] == 0xb0 && img.kbag.key[15] == 0xbf, "key mismatch");
    CHECK(img.has_shsh, "SHSH not noticed");
    CHECK(strcmp(img.vers, "3.1.3") == 0, "vers=\"%s\" expect 3.1.3", img.vers);
    CHECK(img.data_len == 4, "data_len=%u expect 4", img.data_len);
}

/* --- malformed inputs must be rejected, not read out of bounds ----------- */

static void test_reject_too_small(void) {
    img3_t img;
    CHECK(img3_parse(g_buf, 4, &img) == IMG3_ERR_TOO_SMALL, "short buffer accepted");
    CHECK(img3_parse(NULL, 0, &img) == IMG3_ERR_TOO_SMALL, "NULL accepted");
}

static void test_reject_bad_magic(void) {
    img_begin(0x69626f74u);
    put32(0, 0xdeadbeefu);
    img_finish();
    img3_t img;
    CHECK(img3_parse(g_buf, g_len, &img) == IMG3_ERR_BAD_MAGIC, "bad magic accepted");
}

static void test_reject_oversized_fullsize(void) {
    /* fullSize claims far more than the buffer actually holds. */
    img_begin(0x69626f74u);
    img_tag(IMG3_TAG_DATA, "abcd", 4);
    img_finish();
    put32(4, 0xfffffff0u);
    img3_t img;
    CHECK(img3_parse(g_buf, g_len, &img) == IMG3_ERR_BAD_SIZE,
          "oversized fullSize accepted");
}

static void test_reject_tag_past_end(void) {
    /* A tag whose total length runs beyond the container. */
    img_begin(0x69626f74u);
    uint32_t off = img_tag(IMG3_TAG_DATA, "abcd", 4);
    img_finish();
    put32(off + 4, 0x10000u);                     /* absurd tag length */
    img3_t img;
    CHECK(img3_parse(g_buf, g_len, &img) == IMG3_ERR_BAD_TAG, "overlong tag accepted");
}

static void test_reject_tag_data_exceeds_tag(void) {
    /* dataLen larger than the tag that contains it. */
    img_begin(0x69626f74u);
    uint32_t off = img_tag(IMG3_TAG_DATA, "abcd", 4);
    img_finish();
    put32(off + 8, 0x1000u);
    img3_t img;
    CHECK(img3_parse(g_buf, g_len, &img) == IMG3_ERR_BAD_TAG,
          "tag with oversized dataLen accepted");
}

static void test_reject_zero_length_tag(void) {
    /* A tag claiming zero total length would loop forever if not rejected. */
    img_begin(0x69626f74u);
    uint32_t off = img_tag(IMG3_TAG_DATA, "abcd", 4);
    img_finish();
    put32(off + 4, 0);
    img3_t img;
    CHECK(img3_parse(g_buf, g_len, &img) == IMG3_ERR_BAD_TAG,
          "zero-length tag accepted (infinite-loop hazard)");
}

static void test_truncated_kbag_ignored(void) {
    /* A KBAG too short to hold its own fields must simply not be marked
     * present, rather than reading past the tag. */
    uint8_t k[8] = {1,0,0,0, 128,0,0,0};
    img_begin(0x69626f74u);
    img_tag(IMG3_TAG_KBAG, k, sizeof k);
    img_tag(IMG3_TAG_DATA, "abcd", 4);
    img_finish();
    img3_t img;
    CHECK(img3_parse(g_buf, g_len, &img) == IMG3_OK, "parse failed");
    CHECK(!img.kbag.present, "truncated kbag should not be marked present");
}

int main(void) {
    printf("iOS3-VM IMG3 parser tests\n");
    test_parse_minimal();
    test_parse_kbag_and_vers();
    test_reject_too_small();
    test_reject_bad_magic();
    test_reject_oversized_fullsize();
    test_reject_tag_past_end();
    test_reject_tag_data_exceeds_tag();
    test_reject_zero_length_tag();
    test_truncated_kbag_ignored();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

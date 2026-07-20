/*
 * iOS3-VM — Apple device tree parser tests.
 *
 * Builds trees in memory (we ship no Apple firmware) and checks both correct
 * traversal and, importantly, that malformed trees are rejected rather than
 * read out of bounds — device trees arrive inside user-supplied firmware.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "devicetree.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* --------------------------------------------------------- tree building */
static uint8_t  g_dt[4096];
static uint32_t g_len;

static void put32(uint32_t off, uint32_t v) {
    g_dt[off] = (uint8_t)v; g_dt[off+1] = (uint8_t)(v >> 8);
    g_dt[off+2] = (uint8_t)(v >> 16); g_dt[off+3] = (uint8_t)(v >> 24);
}

/* Emit a node header and remember where to patch the counts. */
static uint32_t node_begin(uint32_t nprops, uint32_t nchildren) {
    uint32_t off = g_len;
    put32(off, nprops);
    put32(off + 4, nchildren);
    g_len += 8;
    return off;
}

static void prop_ex(const char *name, const void *val, uint32_t len, uint32_t flags) {
    memset(&g_dt[g_len], 0, DT_PROP_NAME_LEN);
    memcpy(&g_dt[g_len], name, strlen(name));
    g_len += DT_PROP_NAME_LEN;
    put32(g_len, len | flags);      /* Apple stores flags in the top bit */
    g_len += 4;
    if (val && len) memcpy(&g_dt[g_len], val, len);
    g_len += (len + 3u) & ~3u;      /* values are 4-byte padded */
}

static void prop(const char *name, const void *val, uint32_t len) {
    prop_ex(name, val, len, 0u);
}

static void prop_str(const char *name, const char *val) {
    prop(name, val, (uint32_t)strlen(val) + 1u);
}
static void prop_u32(const char *name, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v>>8), (uint8_t)(v>>16), (uint8_t)(v>>24) };
    prop(name, b, 4);
}

/*
 * Build a small tree shaped like a real one:
 *   / (name="device-tree", model)
 *     chosen (name, memory-size)
 *     arm-io (name)
 *       uart0 (name, reg)
 */
static void build_tree(void) {
    memset(g_dt, 0, sizeof g_dt);
    g_len = 0;

    node_begin(2, 2);                       /* root */
    prop_str("name", "device-tree");
    prop_str("model", "iPhone1,1");

    node_begin(2, 0);                       /* chosen */
    prop_str("name", "chosen");
    prop_u32("memory-size", 0x08000000u);

    node_begin(1, 1);                       /* arm-io */
    prop_str("name", "arm-io");

    node_begin(2, 0);                       /* uart0 */
    prop_str("name", "uart0");
    prop_u32("reg", 0x3cc00000u);
}

/* ------------------------------------------------------------- the tests */

static void test_parse_and_root_properties(void) {
    build_tree();
    dt_t dt; dt_node_t root;
    dt_status_t st = dt_parse(g_dt, g_len, &dt, &root);
    CHECK(st == DT_OK, "parse failed: %s", dt_strerror(st));
    CHECK(root.n_props == 2, "root props=%u expect 2", root.n_props);
    CHECK(root.n_children == 2, "root children=%u expect 2", root.n_children);

    const uint8_t *v; uint32_t n;
    CHECK(dt_property(&dt, &root, "model", &v, &n) == DT_OK, "model missing");
    CHECK(n >= 9 && memcmp(v, "iPhone1,1", 9) == 0, "model value wrong");
}

static void test_find_child_and_property(void) {
    build_tree();
    dt_t dt; dt_node_t root, chosen;
    dt_parse(g_dt, g_len, &dt, &root);

    CHECK(dt_child(&dt, &root, "chosen", &chosen) == DT_OK, "chosen not found");
    uint32_t mem = 0;
    CHECK(dt_property_u32(&dt, &chosen, "memory-size", &mem) == DT_OK,
          "memory-size missing");
    CHECK(mem == 0x08000000u, "memory-size=%08x expect 08000000", mem);
}

static void test_path_lookup(void) {
    /* Walking past a sibling and into a grandchild is where offset arithmetic
     * usually goes wrong, so this is the important traversal case. */
    build_tree();
    dt_t dt; dt_node_t root, uart;
    dt_parse(g_dt, g_len, &dt, &root);

    dt_status_t st = dt_path(&dt, &root, "arm-io/uart0", &uart);
    CHECK(st == DT_OK, "path lookup failed: %s", dt_strerror(st));
    uint32_t reg = 0;
    CHECK(dt_property_u32(&dt, &uart, "reg", &reg) == DT_OK, "reg missing");
    CHECK(reg == 0x3cc00000u, "reg=%08x expect 3cc00000 (the UART base)", reg);
    printf("  [device tree] /arm-io/uart0 reg = 0x%08x\n", reg);
}

static void test_missing_lookups_report_not_found(void) {
    build_tree();
    dt_t dt; dt_node_t root, n;
    dt_parse(g_dt, g_len, &dt, &root);
    CHECK(dt_child(&dt, &root, "nope", &n) == DT_ERR_NOT_FOUND, "phantom child found");
    CHECK(dt_property(&dt, &root, "nope", NULL, NULL) == DT_ERR_NOT_FOUND,
          "phantom property found");
    CHECK(dt_path(&dt, &root, "arm-io/nope", &n) == DT_ERR_NOT_FOUND,
          "phantom path found");
}

/* --- malformed trees must be rejected, not read out of bounds ------------ */

static void test_reject_truncated(void) {
    build_tree();
    dt_t dt; dt_node_t root;
    /* Cut the blob in half: the declared structure now runs off the end. */
    CHECK(dt_parse(g_dt, g_len / 2, &dt, &root) == DT_ERR_TRUNCATED,
          "truncated tree accepted");
    CHECK(dt_parse(g_dt, 4, &dt, &root) == DT_ERR_TRUNCATED, "4-byte blob accepted");
    CHECK(dt_parse(NULL, 0, &dt, &root) == DT_ERR_TRUNCATED, "NULL accepted");
}

static void test_reject_absurd_property_length(void) {
    build_tree();
    /* Root's first property claims a gigantic length. */
    put32(8 + DT_PROP_NAME_LEN, 0x7fffffffu);
    dt_t dt; dt_node_t root;
    CHECK(dt_parse(g_dt, g_len, &dt, &root) == DT_ERR_TRUNCATED,
          "absurd property length accepted");
}

static void test_reject_absurd_child_count(void) {
    build_tree();
    put32(4, 0x00ffffffu);                  /* root claims 16M children */
    dt_t dt; dt_node_t root;
    CHECK(dt_parse(g_dt, g_len, &dt, &root) == DT_ERR_TRUNCATED,
          "absurd child count accepted");
}

static void test_reject_excessive_nesting(void) {
    /* A tree nested deeper than DT_MAX_DEPTH must be refused rather than
     * driving unbounded recursion on user-supplied data. */
    memset(g_dt, 0, sizeof g_dt);
    g_len = 0;
    for (unsigned i = 0; i < DT_MAX_DEPTH + 8u; i++) node_begin(0, 1);
    node_begin(0, 0);                       /* innermost leaf */

    dt_t dt; dt_node_t root;
    dt_status_t st = dt_parse(g_dt, g_len, &dt, &root);
    CHECK(st != DT_OK, "excessively nested tree accepted");
}

/*
 * Apple's tooling sets the top bit of a property length as a flag, and the
 * parser masks it off. That is the one piece of Apple-format knowledge in this
 * parser and nothing exercised it — mutation testing showed the whole mask
 * could be deleted with the suite staying green, which would break every real
 * Apple device tree. A flagged tree must parse exactly like an unflagged one,
 * and the reported length must be the masked value.
 */
static void test_property_length_flag_bit(void) {
    static const uint8_t regval[4] = { 0x11, 0x22, 0x33, 0x44 };
    memset(g_dt, 0, sizeof g_dt);
    g_len = 0;
    node_begin(2, 0);
    prop_ex("name", "flagged", 8, 0x80000000u);
    prop_ex("reg", regval, 4, 0x80000000u);

    dt_t dt; dt_node_t root;
    dt_status_t st = dt_parse(g_dt, g_len, &dt, &root);
    CHECK(st == DT_OK, "flagged tree failed to parse: %s", dt_strerror(st));

    const uint8_t *v; uint32_t n;
    CHECK(dt_property(&dt, &root, "name", &v, &n) == DT_OK, "flagged name missing");
    CHECK(n == 8, "length=%u expect 8 (the flag bit must be masked off)", n);

    uint32_t reg = 0;
    CHECK(dt_property_u32(&dt, &root, "reg", &reg) == DT_OK, "flagged reg missing");
    CHECK(reg == 0x44332211u, "reg=%08x expect 44332211", reg);
}

int main(void) {
    printf("iOS3-VM device tree tests\n");
    test_parse_and_root_properties();
    test_find_child_and_property();
    test_path_lookup();
    test_missing_lookups_report_not_found();
    test_reject_truncated();
    test_reject_absurd_property_length();
    test_reject_absurd_child_count();
    test_reject_excessive_nesting();
    test_property_length_flag_bit();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

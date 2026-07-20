/*
 * iOS3-VM — firmware loader tests.
 *
 * The headline test is the whole M3 pipeline in miniature: build an
 * AES-encrypted IMG3 whose payload is real ARM code, then decrypt it, load it
 * into guest RAM and execute it on the emulated S5L8900 until it prints over
 * the UART. That is exactly the path Apple's LLB/iBoot will take — the only
 * difference being who wrote the payload.
 *
 * We ship no Apple firmware, so every image here is built in memory.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "loader.h"
#include "aes.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("  FAIL %s:%d: ", __func__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ------------------------------------------------------- IMG3 construction */
static uint8_t  g_img[4096];
static uint32_t g_len;

static void put32(uint32_t off, uint32_t v) {
    g_img[off] = (uint8_t)v; g_img[off+1] = (uint8_t)(v >> 8);
    g_img[off+2] = (uint8_t)(v >> 16); g_img[off+3] = (uint8_t)(v >> 24);
}
static void img_begin(uint32_t ident) {
    memset(g_img, 0, sizeof g_img);
    put32(0, 0x33676d49u);
    put32(16, ident);
    g_len = 20;
}
static void img_tag(uint32_t magic, const void *data, uint32_t n) {
    uint32_t off = g_len;
    put32(off, magic); put32(off + 4, 12 + n); put32(off + 8, n);
    if (data && n) memcpy(&g_img[off + 12], data, n);
    g_len += 12 + n;
}
static void img_finish(void) {
    put32(4, g_len); put32(8, g_len - 20); put32(12, g_len - 20);
}

/*
 * The payload: ARM code that prints "iBoot" over the emulated UART and parks.
 * It reads the UART base from a literal exactly as real firmware does.
 */
static const uint32_t PAYLOAD[] = {
    0xe59f0028u,          /* LDR r0,[pc,#40]     -> UART base   */
    0xe3a01069u,          /* MOV r1,#'i' */  0xe5801020u,
    0xe3a01042u,          /* MOV r1,#'B' */  0xe5801020u,
    0xe3a0106fu,          /* MOV r1,#'o' */  0xe5801020u,
    0xe3a0106fu,          /* MOV r1,#'o' */  0xe5801020u,
    0xe3a01074u,          /* MOV r1,#'t' */  0xe5801020u,
    0xeafffffeu,          /* B .                                */
    S5L8900_UART0_BASE    /* literal at offset 0x30             */
};

static void test_plain_image_boots(void) {
    img_begin(0x69626f74u);                       /* 'ibot' */
    img_tag(0x44415441u, PAYLOAD, (uint32_t)sizeof PAYLOAD);   /* DATA */
    img_finish();

    s5l8900_t m;
    CHECK(s5l8900_init(&m, 0, 1u << 20), "machine init failed");

    fw_image_t info;
    fw_status_t st = fw_boot_img3(&m, g_img, g_len, NULL, 0, 0x1000, &info);
    CHECK(st == FW_OK, "boot failed: %s", fw_strerror(st));
    CHECK(strcmp(info.ident_str, "ibot") == 0, "ident=%s expect ibot", info.ident_str);
    CHECK(!info.was_encrypted, "plain image reported as encrypted");

    arm_status_t ast = ARM_OK;
    s5l8900_run(&m, 64, &ast);
    m.uart0.tx[m.uart0.tx_len] = '\0';
    CHECK(strcmp(m.uart0.tx, "iBoot") == 0,
          "uart=\"%s\" expect \"iBoot\"", m.uart0.tx);
    printf("  [plain IMG3 -> guest said] %s\n", m.uart0.tx);
    s5l8900_free(&m);
}

static void test_encrypted_image_boots(void) {
    /* Encrypt the payload exactly as Apple does, and give the loader the key. */
    static const uint8_t key[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    uint8_t iv[16];
    for (unsigned i = 0; i < 16; i++) iv[i] = (uint8_t)(0x20 + i);

    /* Real payloads are not always block-aligned, and img3_decrypt_data covers
     * the aligned prefix while passing any tail through. Mirror that here so
     * the test exercises the unaligned path rather than avoiding it. */
    uint8_t cipher[sizeof PAYLOAD];
    const uint32_t whole = (uint32_t)(sizeof PAYLOAD) & ~15u;
    const uint32_t tail  = (uint32_t)(sizeof PAYLOAD) - whole;
    aes_ctx_t ctx;
    aes_init(&ctx, key, 128);
    CHECK(aes_cbc_encrypt(&ctx, iv, (const uint8_t *)PAYLOAD, cipher, whole),
          "cbc encrypt of the aligned prefix failed");
    if (tail) memcpy(cipher + whole, (const uint8_t *)PAYLOAD + whole, tail);

    uint8_t kbag[24 + 16];
    memset(kbag, 0, sizeof kbag);
    kbag[0] = 1; kbag[4] = 128;
    memcpy(&kbag[8], iv, 16);

    img_begin(0x69626f74u);
    img_tag(0x4b424147u, kbag, sizeof kbag);                   /* KBAG */
    img_tag(0x44415441u, cipher, (uint32_t)sizeof cipher);     /* DATA */
    img_tag(0x53485348u, "sig", 3);                            /* SHSH */
    img_finish();

    s5l8900_t m;
    s5l8900_init(&m, 0, 1u << 20);

    fw_image_t info;
    fw_status_t st = fw_boot_img3(&m, g_img, g_len, key, 128, 0x1000, &info);
    CHECK(st == FW_OK, "boot failed: %s", fw_strerror(st));
    CHECK(info.was_encrypted, "encrypted image not reported as encrypted");
    CHECK(info.was_signed, "SHSH present but not reported");

    arm_status_t ast = ARM_OK;
    s5l8900_run(&m, 64, &ast);
    m.uart0.tx[m.uart0.tx_len] = '\0';
    CHECK(strcmp(m.uart0.tx, "iBoot") == 0,
          "uart=\"%s\" expect \"iBoot\" (decrypted code executed)", m.uart0.tx);
    printf("  [encrypted IMG3 -> decrypted -> guest said] %s\n", m.uart0.tx);
    s5l8900_free(&m);
}

static void test_encrypted_without_key_is_refused(void) {
    uint8_t kbag[24 + 16];
    memset(kbag, 0, sizeof kbag);
    kbag[0] = 1; kbag[4] = 128;

    img_begin(0x69626f74u);
    img_tag(0x4b424147u, kbag, sizeof kbag);
    img_tag(0x44415441u, PAYLOAD, (uint32_t)sizeof PAYLOAD);
    img_finish();

    s5l8900_t m;
    s5l8900_init(&m, 0, 1u << 20);
    fw_status_t st = fw_load_img3(&m, g_img, g_len, NULL, 0, 0x1000, NULL);
    CHECK(st == FW_ERR_KEY_REQUIRED, "expected FW_ERR_KEY_REQUIRED, got %s",
          fw_strerror(st));
    s5l8900_free(&m);
}

static void test_oversized_payload_refused(void) {
    /* A payload that does not fit must be refused before anything is written. */
    img_begin(0x69626f74u);
    img_tag(0x44415441u, PAYLOAD, (uint32_t)sizeof PAYLOAD);
    img_finish();

    s5l8900_t m;
    s5l8900_init(&m, 0, 1u << 12);               /* only 4 KiB of RAM */
    fw_status_t st = fw_load_img3(&m, g_img, g_len, NULL, 0, 0xff0, NULL);
    CHECK(st == FW_ERR_NO_ROOM, "expected FW_ERR_NO_ROOM, got %s", fw_strerror(st));
    s5l8900_free(&m);
}

static void test_garbage_refused(void) {
    uint8_t junk[64];
    memset(junk, 0xA5, sizeof junk);
    s5l8900_t m;
    s5l8900_init(&m, 0, 1u << 16);
    fw_status_t st = fw_load_img3(&m, junk, sizeof junk, NULL, 0, 0x100, NULL);
    CHECK(st == FW_ERR_PARSE, "expected FW_ERR_PARSE, got %s", fw_strerror(st));
    s5l8900_free(&m);
}

int main(void) {
    printf("iOS3-VM firmware loader tests\n");
    test_plain_image_boots();
    test_encrypted_image_boots();
    test_encrypted_without_key_is_refused();
    test_oversized_payload_refused();
    test_garbage_refused();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

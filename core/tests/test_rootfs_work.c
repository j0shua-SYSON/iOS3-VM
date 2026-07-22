/*
 * iOS3-VM -- bounded external rootfs work-image provisioner tests.
 *
 * Fixtures are intentionally tiny bare HFSX volumes (8 KiB before growth),
 * and every path is relative to the F:-backed test working directory.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#else
#define _POSIX_C_SOURCE 200809L
#endif

#include "rootfs_work.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define FIXTURE_BLOCK_SIZE 512u
#define FIXTURE_BLOCKS 16u
#define FIXTURE_SIZE (FIXTURE_BLOCK_SIZE * FIXTURE_BLOCKS)
#define FIXTURE_BITMAP_OFFSET (4u * FIXTURE_BLOCK_SIZE)
#define FIXTURE_FSTAB_OFFSET 4090u
#define HFS_VH_OFF 1024u
#define HFS_VH_LEN 512u

static const uint8_t FSTAB_STOCK[] =
    "/dev/disk0s1 / hfs ro 0 1\n"
    "/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2\n";

/* Independently fixed SHA-256 of make_hfs_fixture(..., 1). */
static const uint8_t FIXTURE_SHA256[IOS3_SHA256_DIGEST_SIZE] = {
    0xa2u, 0x95u, 0x22u, 0x37u, 0x9bu, 0x5eu, 0xf1u, 0x8fu,
    0x60u, 0x79u, 0x24u, 0x54u, 0x3au, 0xb6u, 0x14u, 0xb9u,
    0x87u, 0x32u, 0x31u, 0xb0u, 0x58u, 0x1bu, 0xbfu, 0x95u,
    0xf5u, 0xc7u, 0xa1u, 0x49u, 0x6au, 0xcfu, 0x5du, 0x47u
};

static const uint8_t SHA256_EMPTY[IOS3_SHA256_DIGEST_SIZE] = {
    0xe3u, 0xb0u, 0xc4u, 0x42u, 0x98u, 0xfcu, 0x1cu, 0x14u,
    0x9au, 0xfbu, 0xf4u, 0xc8u, 0x99u, 0x6fu, 0xb9u, 0x24u,
    0x27u, 0xaeu, 0x41u, 0xe4u, 0x64u, 0x9bu, 0x93u, 0x4cu,
    0xa4u, 0x95u, 0x99u, 0x1bu, 0x78u, 0x52u, 0xb8u, 0x55u
};

static const uint8_t SHA256_ABC[IOS3_SHA256_DIGEST_SIZE] = {
    0xbau, 0x78u, 0x16u, 0xbfu, 0x8fu, 0x01u, 0xcfu, 0xeau,
    0x41u, 0x41u, 0x40u, 0xdeu, 0x5du, 0xaeu, 0x22u, 0x23u,
    0xb0u, 0x03u, 0x61u, 0xa3u, 0x96u, 0x17u, 0x7au, 0x9cu,
    0xb4u, 0x10u, 0xffu, 0x61u, 0xf2u, 0x00u, 0x15u, 0xadu
};

static const uint8_t SHA256_LONG[IOS3_SHA256_DIGEST_SIZE] = {
    0x24u, 0x8du, 0x6au, 0x61u, 0xd2u, 0x06u, 0x38u, 0xb8u,
    0xe5u, 0xc0u, 0x26u, 0x93u, 0x0cu, 0x3eu, 0x60u, 0x39u,
    0xa3u, 0x3cu, 0xe4u, 0x59u, 0x64u, 0xffu, 0x21u, 0x67u,
    0xf6u, 0xecu, 0xedu, 0xd4u, 0x19u, 0xdbu, 0x06u, 0xc1u
};

static int g_pass;
static int g_fail;
static unsigned g_serial;

#define CHECK(condition, ...) do {                                           \
    if (condition) {                                                         \
        g_pass++;                                                            \
    } else {                                                                \
        g_fail++;                                                            \
        printf("  FAIL %s:%d: ", __func__, __LINE__);                       \
        printf(__VA_ARGS__);                                                 \
        printf("\n");                                                       \
    }                                                                        \
} while (0)

static unsigned long process_id(void) {
#ifdef _WIN32
    return (unsigned long)_getpid();
#else
    return (unsigned long)getpid();
#endif
}

static bool make_path(char *path, size_t capacity, const char *tag) {
    int length;

    g_serial++;
    length = snprintf(path, capacity, "rootfs_work_%lu_%u_%s.img",
                      process_id(), g_serial, tag);
    return length > 0 && (size_t)length < capacity;
}

static void put_be16(uint8_t *bytes, uint16_t value) {
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static void put_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void put_be64(uint8_t *bytes, uint64_t value) {
    put_be32(bytes, (uint32_t)(value >> 32));
    put_be32(bytes + 4, (uint32_t)value);
}

static uint32_t get_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | bytes[3];
}

static void bitmap_set(uint8_t *image, uint32_t bit, bool set) {
    uint8_t *byte = image + FIXTURE_BITMAP_OFFSET + (bit >> 3);
    uint8_t mask = (uint8_t)(1u << (7u - (bit & 7u)));

    *byte = set ? (uint8_t)(*byte | mask) :
                  (uint8_t)(*byte & (uint8_t)~mask);
}

static void make_hfs_fixture(uint8_t image[FIXTURE_SIZE], unsigned fstab_count) {
    uint8_t *header;
    unsigned index;
    /* First 1536 bytes, allocation file, and final 1024 bytes are reserved. */
    const uint32_t used[] = {0u, 1u, 2u, 4u, 14u, 15u};

    memset(image, 0, FIXTURE_SIZE);
    header = image + HFS_VH_OFF;
    put_be16(header, 0x4858u);             /* HFSX */
    put_be16(header + 2, 5u);
    put_be32(header + 4, 1u << 8);         /* cleanly unmounted */
    put_be32(header + 12, 0u);
    put_be32(header + 40, FIXTURE_BLOCK_SIZE);
    put_be32(header + 44, FIXTURE_BLOCKS);
    put_be32(header + 48, FIXTURE_BLOCKS -
                                 (uint32_t)(sizeof(used) / sizeof(used[0])));
    put_be32(header + 52, 1u);
    put_be64(header + 112, 8u);            /* 64 bitmap bits */
    put_be32(header + 124, 1u);            /* one allocation-file block */
    put_be32(header + 128, 4u);            /* extent start */
    put_be32(header + 132, 1u);            /* extent count */
    for (index = 0; index < sizeof(used) / sizeof(used[0]); index++)
        bitmap_set(image, used[index], true);
    if (fstab_count >= 1u)
        memcpy(image + FIXTURE_FSTAB_OFFSET, FSTAB_STOCK,
               sizeof(FSTAB_STOCK) - 1u);
    if (fstab_count >= 2u)
        memcpy(image + 5200u, FSTAB_STOCK, sizeof(FSTAB_STOCK) - 1u);
    memcpy(image + FIXTURE_SIZE - HFS_VH_OFF, header, HFS_VH_LEN);
}

static bool write_file(const char *path, const uint8_t *bytes, size_t size) {
    FILE *stream = fopen(path, "wb");
    bool okay = stream != NULL;

    if (okay && size != 0u && fwrite(bytes, 1u, size, stream) != size)
        okay = false;
    if (stream && fclose(stream) != 0)
        okay = false;
    return okay;
}

static bool read_file(const char *path, uint8_t *bytes, size_t size) {
    FILE *stream = fopen(path, "rb");
    bool okay = stream != NULL;

    if (okay && size != 0u && fread(bytes, 1u, size, stream) != size)
        okay = false;
    if (stream && fclose(stream) != 0)
        okay = false;
    return okay;
}

static uint64_t file_size(const char *path) {
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &info))
        return UINT64_MAX;
    return ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
#else
    struct stat info;
    if (stat(path, &info) != 0 || info.st_size < 0)
        return UINT64_MAX;
    return (uint64_t)info.st_size;
#endif
}

static bool path_exists(const char *path) {
#ifdef _WIN32
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat info;
    return lstat(path, &info) == 0;
#endif
}

static void remove_if_present(const char *path) {
    if (path)
        (void)remove(path);
}

static void require_fixture_identity(rootfs_work_options_t *options) {
    options->source_identity.required = true;
    options->source_identity.expected_size = FIXTURE_SIZE;
    memcpy(options->source_identity.expected_sha256, FIXTURE_SHA256,
           sizeof(FIXTURE_SHA256));
}

static void test_sha256_known_answers_and_chunking(void) {
    static const uint8_t abc[] = {'a', 'b', 'c'};
    static const uint8_t long_message[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    ios3_sha256_context_t context;
    uint8_t digest[IOS3_SHA256_DIGEST_SIZE];
    uint8_t in_place[IOS3_SHA256_DIGEST_SIZE];
    size_t index;

    CHECK(ios3_sha256(NULL, 0u, digest) &&
          memcmp(digest, SHA256_EMPTY, sizeof(digest)) == 0,
          "empty SHA-256 known-answer test failed");
    CHECK(ios3_sha256(abc, sizeof(abc), digest) &&
          memcmp(digest, SHA256_ABC, sizeof(digest)) == 0,
          "abc SHA-256 known-answer test failed");
    memset(in_place, 0, sizeof(in_place));
    memcpy(in_place, abc, sizeof(abc));
    CHECK(ios3_sha256(in_place, sizeof(abc), in_place) &&
          memcmp(in_place, SHA256_ABC, sizeof(in_place)) == 0,
          "one-shot SHA-256 did not support exact in-place output overlap");
    CHECK(ios3_sha256(long_message, sizeof(long_message) - 1u, digest) &&
          memcmp(digest, SHA256_LONG, sizeof(digest)) == 0,
          "long SHA-256 known-answer test failed");

    CHECK(ios3_sha256_init(&context), "chunked SHA-256 init failed");
    for (index = 0u; index < sizeof(long_message) - 1u; index++)
        CHECK(ios3_sha256_update(&context, long_message + index, 1u),
              "one-byte SHA-256 update %zu failed", index);
    CHECK(ios3_sha256_final(&context, digest) &&
          memcmp(digest, SHA256_LONG, sizeof(digest)) == 0,
          "one-byte chunked SHA-256 differs from the KAT");
}

static void test_sha256_invalid_and_overflow_guards(void) {
    ios3_sha256_context_t context;
    ios3_sha256_context_t before;
    uint8_t digest[IOS3_SHA256_DIGEST_SIZE];
    uint8_t byte = 0x5au;

    CHECK(!ios3_sha256_init(NULL), "NULL SHA-256 context was initialized");
    CHECK(!ios3_sha256(NULL, 1u, digest),
          "NULL non-empty one-shot input was accepted");
    CHECK(!ios3_sha256(&byte, 1u, NULL),
          "NULL one-shot digest was accepted");
#if SIZE_MAX > (UINT64_MAX / UINT64_C(8))
    CHECK(!ios3_sha256(&byte, SIZE_MAX, digest),
          "oversized one-shot SHA-256 input was accepted");
#endif

    CHECK(ios3_sha256_init(&context), "invalid-case init failed");
    before = context;
    CHECK(!ios3_sha256_update(&context, NULL, 1u) &&
          memcmp(&context, &before, sizeof(context)) == 0,
          "invalid non-empty update mutated the SHA-256 context");
    CHECK(ios3_sha256_update(&context, NULL, 0u) &&
          memcmp(&context, &before, sizeof(context)) == 0,
          "zero-length update was not a no-op");
    CHECK(!ios3_sha256_final(&context, NULL) &&
          memcmp(&context, &before, sizeof(context)) == 0,
          "invalid final mutated the SHA-256 context");

    context.total_bytes = IOS3_SHA256_MAX_INPUT_BYTES;
    before = context;
    CHECK(!ios3_sha256_update(&context, &byte, 1u) &&
          memcmp(&context, &before, sizeof(context)) == 0,
          "cumulative SHA-256 overflow mutated the context");
    context.total_bytes = IOS3_SHA256_MAX_INPUT_BYTES - UINT64_C(1);
    before = context;
    CHECK(!ios3_sha256_update(&context, digest, 2u) &&
          memcmp(&context, &before, sizeof(context)) == 0,
          "multi-byte SHA-256 overflow mutated the context");
    context.total_bytes = IOS3_SHA256_MAX_INPUT_BYTES + UINT64_C(1);
    before = context;
    CHECK(!ios3_sha256_update(&context, NULL, 0u) &&
          memcmp(&context, &before, sizeof(context)) == 0,
          "already-overflowed SHA-256 context was accepted");

    CHECK(ios3_sha256_init(&context) &&
          ios3_sha256_update(&context, &byte, 1u) &&
          ios3_sha256_final(&context, digest),
          "finalization setup failed");
    before = context;
    CHECK(!ios3_sha256_update(&context, &byte, 1u) &&
          memcmp(&context, &before, sizeof(context)) == 0,
          "post-final update mutated the SHA-256 context");
    CHECK(!ios3_sha256_final(&context, digest) &&
          memcmp(&context, &before, sizeof(context)) == 0,
          "second final mutated the SHA-256 context");
}

static bool no_temporary_files(void) {
    unsigned attempt;

    for (attempt = 0; attempt < 128u; attempt++) {
        char path[100];
        int length = snprintf(path, sizeof(path),
                              ".rootfs-work-%lu-%u.tmp",
                              process_id(), attempt);
        if (length <= 0 || (size_t)length >= sizeof(path) || path_exists(path))
            return false;
    }
    return true;
}

static void test_success_boundary_growth_and_source_immutable(void) {
    char source[160];
    char destination[160];
    uint8_t original[FIXTURE_SIZE];
    uint8_t source_after[FIXTURE_SIZE];
    uint8_t header[HFS_VH_LEN];
    uint8_t alternate[HFS_VH_LEN];
    uint8_t replacement[sizeof(FSTAB_STOCK) - 1u];
    rootfs_work_options_t options;
    rootfs_work_result_t result;
    rootfs_work_status_t status;
    FILE *stream;
    uint64_t expected_size = (uint64_t)19u * FIXTURE_BLOCK_SIZE;

    CHECK(make_path(source, sizeof(source), "source"),
          "could not form source path");
    CHECK(make_path(destination, sizeof(destination), "work"),
          "could not form destination path");
    remove_if_present(source);
    remove_if_present(destination);
    make_hfs_fixture(original, 1u);
    CHECK(write_file(source, original, sizeof(original)),
          "could not write valid fixture");
    memset(&options, 0, sizeof(options));
    options.fstab_line = "/dev/md0 / hfs rw,update 0 0";
    options.growth_bytes = 4u * FIXTURE_BLOCK_SIZE;
    options.io_buffer_bytes = 1u; /* worst-case copy/hash/scan boundaries */
    require_fixture_identity(&options);
    status = rootfs_work_create(source, destination, &options, &result);
    CHECK(status == ROOTFS_WORK_OK,
          "create failed: %s/%s sys=%d detail=%s",
          rootfs_work_status_name(status), rootfs_work_stage_name(result.stage),
          result.system_error, result.detail);
    CHECK(result.published && !result.temporary_left,
          "success did not report one clean publication");
    CHECK(result.source_sha256_valid && result.source_identity_verified &&
          memcmp(result.source_sha256, FIXTURE_SHA256,
                 sizeof(FIXTURE_SHA256)) == 0,
          "successful exact source identity was not reported");
    CHECK(result.source_size == FIXTURE_SIZE &&
          result.bytes_copied == FIXTURE_SIZE,
          "copy accounting was %llu/%llu",
          (unsigned long long)result.source_size,
          (unsigned long long)result.bytes_copied);
    CHECK(result.final_size == expected_size &&
          file_size(destination) == expected_size,
          "grown size was %llu (disk %llu), expected %llu",
          (unsigned long long)result.final_size,
          (unsigned long long)file_size(destination),
          (unsigned long long)expected_size);
    CHECK(result.io_buffer_bytes == 1u &&
          result.io_buffer_bytes <= ROOTFS_WORK_MAX_IO_BUFFER,
          "bounded I/O buffer accounting was %zu", result.io_buffer_bytes);
    CHECK(result.fstab_offset == FIXTURE_FSTAB_OFFSET,
          "boundary-spanning fstab hit was 0x%llx",
          (unsigned long long)result.fstab_offset);
    CHECK(read_file(source, source_after, sizeof(source_after)) &&
          memcmp(original, source_after, sizeof(original)) == 0,
          "immutable source changed during provisioning");

    stream = fopen(destination, "rb");
    CHECK(stream != NULL, "could not open published work image");
    if (stream) {
        CHECK(fseek(stream, (long)HFS_VH_OFF, SEEK_SET) == 0 &&
              fread(header, 1u, sizeof(header), stream) == sizeof(header),
              "could not read grown primary header");
        CHECK(fseek(stream, (long)(expected_size - HFS_VH_OFF), SEEK_SET) == 0 &&
              fread(alternate, 1u, sizeof(alternate), stream) ==
                  sizeof(alternate),
              "could not read grown alternate header");
        CHECK(fseek(stream, (long)FIXTURE_FSTAB_OFFSET, SEEK_SET) == 0 &&
              fread(replacement, 1u, sizeof(replacement), stream) ==
                  sizeof(replacement),
              "could not read rewritten fstab");
        CHECK(fclose(stream) == 0, "could not close published work image");
        CHECK(get_be32(header + 44) == 19u,
              "grown totalBlocks was %u", get_be32(header + 44));
        CHECK(get_be32(header + 48) == 13u,
              "grown freeBlocks was %u", get_be32(header + 48));
        CHECK(get_be32(header + 52) == 14u,
              "grown nextAllocation was %u", get_be32(header + 52));
        CHECK(memcmp(header, alternate, sizeof(header)) == 0,
              "grown alternate header differs from primary");
        CHECK(memcmp(replacement, options.fstab_line,
                     strlen(options.fstab_line)) == 0 &&
              replacement[strlen(options.fstab_line)] == '\n' &&
              replacement[sizeof(replacement) - 1u] == '\n',
              "fstab replacement/padding is malformed");
    }
    CHECK(no_temporary_files(), "successful publication left a temp name");
    remove_if_present(destination);
    remove_if_present(source);
}

static void test_required_identity_chunk_boundaries(void) {
    static const size_t buffer_sizes[] = {1u, 63u, 64u, 65u};
    char source[160];
    uint8_t fixture[FIXTURE_SIZE];
    size_t index;

    CHECK(make_path(source, sizeof(source), "identity-boundary-source"),
          "could not form identity-boundary source path");
    remove_if_present(source);
    make_hfs_fixture(fixture, 1u);
    CHECK(write_file(source, fixture, sizeof(fixture)),
          "could not write identity-boundary fixture");

    for (index = 0u; index < sizeof(buffer_sizes) / sizeof(buffer_sizes[0]);
         index++) {
        char destination[160];
        rootfs_work_options_t options;
        rootfs_work_result_t result;
        rootfs_work_status_t status;

        CHECK(make_path(destination, sizeof(destination),
                        "identity-boundary-work"),
              "could not form identity-boundary destination %zu", index);
        remove_if_present(destination);
        memset(&options, 0, sizeof(options));
        require_fixture_identity(&options);
        options.io_buffer_bytes = buffer_sizes[index];
        status = rootfs_work_create(source, destination, &options, &result);
        CHECK(status == ROOTFS_WORK_OK,
              "required identity with %zu-byte chunks returned %s/%s: %s",
              buffer_sizes[index], rootfs_work_status_name(status),
              rootfs_work_stage_name(result.stage), result.detail);
        CHECK(result.published && result.source_sha256_valid &&
                  result.source_identity_verified &&
                  result.io_buffer_bytes == buffer_sizes[index] &&
                  memcmp(result.source_sha256, FIXTURE_SHA256,
                         sizeof(FIXTURE_SHA256)) == 0,
              "required identity with %zu-byte chunks reported wrong proof",
              buffer_sizes[index]);
        CHECK(path_exists(destination) &&
                  file_size(destination) == FIXTURE_SIZE &&
                  !result.temporary_left && no_temporary_files(),
              "required identity with %zu-byte chunks did not publish cleanly",
              buffer_sizes[index]);
        remove_if_present(destination);
    }

    remove_if_present(source);
}

static void test_source_identity_policy_and_cleanup(void) {
    char source[160];
    char wrong_size_destination[160];
    char wrong_hash_destination[160];
    char disabled_destination[160];
    uint8_t fixture[FIXTURE_SIZE];
    rootfs_work_options_t options;
    rootfs_work_result_t result;

    CHECK(make_path(source, sizeof(source), "identity-source"),
          "could not form identity source path");
    CHECK(make_path(wrong_size_destination,
                    sizeof(wrong_size_destination), "wrong-size"),
          "could not form wrong-size destination path");
    CHECK(make_path(wrong_hash_destination,
                    sizeof(wrong_hash_destination), "wrong-hash"),
          "could not form wrong-hash destination path");
    CHECK(make_path(disabled_destination,
                    sizeof(disabled_destination), "identity-disabled"),
          "could not form disabled-policy destination path");
    remove_if_present(source);
    remove_if_present(wrong_size_destination);
    remove_if_present(wrong_hash_destination);
    remove_if_present(disabled_destination);
    make_hfs_fixture(fixture, 1u);
    CHECK(write_file(source, fixture, sizeof(fixture)),
          "could not write identity fixture");

    memset(&options, 0, sizeof(options));
    require_fixture_identity(&options);
    options.source_identity.expected_size = FIXTURE_SIZE + UINT64_C(1);
    options.io_buffer_bytes = 63u;
    CHECK(rootfs_work_create(source, wrong_size_destination,
                             &options, &result) ==
              ROOTFS_WORK_SOURCE_IDENTITY_MISMATCH &&
          result.stage == ROOTFS_WORK_STAGE_SOURCE_IDENTITY,
          "wrong source size returned %s/%s: %s",
          rootfs_work_status_name(result.status),
          rootfs_work_stage_name(result.stage), result.detail);
    CHECK(result.bytes_copied == 0u && !result.source_sha256_valid &&
          !result.source_identity_verified && !result.published,
          "wrong-size refusal reported copy/hash/publication progress");
    CHECK(!path_exists(wrong_size_destination) && no_temporary_files(),
          "wrong-size identity refusal left an output artifact");

    memset(&options, 0, sizeof(options));
    require_fixture_identity(&options);
    options.source_identity.expected_sha256[31] ^= 1u;
    options.io_buffer_bytes = 64u;
    CHECK(rootfs_work_create(source, wrong_hash_destination,
                             &options, &result) ==
              ROOTFS_WORK_SOURCE_IDENTITY_MISMATCH &&
          result.stage == ROOTFS_WORK_STAGE_SOURCE_IDENTITY,
          "wrong source digest returned %s/%s: %s",
          rootfs_work_status_name(result.status),
          rootfs_work_stage_name(result.stage), result.detail);
    CHECK(result.bytes_copied == FIXTURE_SIZE &&
          result.source_sha256_valid &&
          !result.source_identity_verified && !result.published &&
          memcmp(result.source_sha256, FIXTURE_SHA256,
                 sizeof(FIXTURE_SHA256)) == 0,
          "wrong-hash refusal flags or observed digest are incorrect");
    CHECK(!path_exists(wrong_hash_destination) && no_temporary_files(),
          "wrong-hash identity refusal left an output artifact");

    memset(&options, 0, sizeof(options));
    options.source_identity.required = false;
    options.source_identity.expected_size = UINT64_MAX;
    memset(options.source_identity.expected_sha256, 0x5a,
           sizeof(options.source_identity.expected_sha256));
    options.io_buffer_bytes = 65u;
    CHECK(rootfs_work_create(source, disabled_destination,
                             &options, &result) == ROOTFS_WORK_OK,
          "disabled identity policy returned %s/%s: %s",
          rootfs_work_status_name(result.status),
          rootfs_work_stage_name(result.stage), result.detail);
    CHECK(result.published && result.source_sha256_valid &&
          !result.source_identity_verified &&
          memcmp(result.source_sha256, FIXTURE_SHA256,
                 sizeof(FIXTURE_SHA256)) == 0,
          "disabled identity policy did not report digest-only semantics");
    CHECK(path_exists(disabled_destination) && no_temporary_files(),
          "disabled identity policy publication state is wrong");

    remove_if_present(disabled_destination);
    remove_if_present(wrong_hash_destination);
    remove_if_present(wrong_size_destination);
    remove_if_present(source);
}

static void expect_fstab_failure(unsigned count,
                                 rootfs_work_status_t expected_status,
                                 const char *tag) {
    char source[160];
    char destination[160];
    uint8_t fixture[FIXTURE_SIZE];
    rootfs_work_options_t options;
    rootfs_work_result_t result;

    CHECK(make_path(source, sizeof(source), tag), "could not form source path");
    CHECK(make_path(destination, sizeof(destination), "rejected"),
          "could not form destination path");
    remove_if_present(source);
    remove_if_present(destination);
    make_hfs_fixture(fixture, count);
    CHECK(write_file(source, fixture, FIXTURE_SIZE),
          "could not write fstab fixture");
    memset(&options, 0, sizeof(options));
    options.io_buffer_bytes = 19u;
    CHECK(rootfs_work_create(source, destination, &options, &result) ==
              expected_status,
          "fstab count %u returned %s at %s: %s", count,
          rootfs_work_status_name(result.status),
          rootfs_work_stage_name(result.stage), result.detail);
    CHECK(!result.published && !path_exists(destination),
          "rejected fstab fixture published a destination");
    CHECK(no_temporary_files(),
          "rejected fstab fixture left an unpublished temp");
    remove_if_present(destination);
    remove_if_present(source);
}

static void test_fstab_uniqueness_and_cleanup(void) {
    expect_fstab_failure(0u, ROOTFS_WORK_FSTAB_NOT_UNIQUE, "absent");
    expect_fstab_failure(2u, ROOTFS_WORK_FSTAB_NOT_UNIQUE, "duplicate");
}

static void expect_hfs_invalid(const uint8_t fixture[FIXTURE_SIZE],
                               const char *tag, const char *detail_fragment) {
    char source[160];
    char destination[160];
    rootfs_work_result_t result;

    CHECK(make_path(source, sizeof(source), tag),
          "could not form malformed source path");
    CHECK(make_path(destination, sizeof(destination), "malformed-work"),
          "could not form malformed destination path");
    remove_if_present(source);
    remove_if_present(destination);
    CHECK(write_file(source, fixture, FIXTURE_SIZE),
          "could not write malformed fixture");
    CHECK(rootfs_work_create(source, destination, NULL, &result) ==
              ROOTFS_WORK_HFS_INVALID,
          "malformed HFS returned %s/%s: %s",
          rootfs_work_status_name(result.status),
          rootfs_work_stage_name(result.stage), result.detail);
    CHECK(strstr(result.detail, detail_fragment) != NULL,
          "malformed HFS detail '%s' lacks '%s'", result.detail,
          detail_fragment);
    CHECK(!path_exists(destination) && no_temporary_files(),
          "malformed HFS left output artifacts");
    remove_if_present(destination);
    remove_if_present(source);
}

static void test_malformed_hfs_refused(void) {
    uint8_t fixture[FIXTURE_SIZE];
    uint8_t *header;

    make_hfs_fixture(fixture, 1u);
    fixture[FIXTURE_SIZE - HFS_VH_OFF + 48u] ^= 1u;
    expect_hfs_invalid(fixture, "alternate-mismatch", "headers disagree");

    make_hfs_fixture(fixture, 1u);
    bitmap_set(fixture, 1u, false);
    expect_hfs_invalid(fixture, "free-head", "required metadata block 1");

    make_hfs_fixture(fixture, 1u);
    bitmap_set(fixture, 14u, false);
    expect_hfs_invalid(fixture, "free-tail", "required metadata block 14");

    make_hfs_fixture(fixture, 1u);
    bitmap_set(fixture, 4u, false);
    expect_hfs_invalid(fixture, "free-allocation-file",
                       "required metadata block 4");

    make_hfs_fixture(fixture, 1u);
    header = fixture + HFS_VH_OFF;
    put_be32(header + 136u, 6u); /* extent[1].start with count == 0 */
    memcpy(fixture + FIXTURE_SIZE - HFS_VH_OFF, header, HFS_VH_LEN);
    expect_hfs_invalid(fixture, "noncanonical-empty-extent",
                       "empty allocation extent 1");

    make_hfs_fixture(fixture, 1u);
    header = fixture + HFS_VH_OFF;
    put_be32(header + 124u, 2u);
    put_be32(header + 144u, 6u); /* extent[2] after empty extent[1] */
    put_be32(header + 148u, 1u);
    memcpy(fixture + FIXTURE_SIZE - HFS_VH_OFF, header, HFS_VH_LEN);
    expect_hfs_invalid(fixture, "gapped-extents",
                       "follows an empty inline extent");

    make_hfs_fixture(fixture, 1u);
    header = fixture + HFS_VH_OFF;
    put_be32(header + 4u, (1u << 8) | (1u << 15));
    memcpy(fixture + FIXTURE_SIZE - HFS_VH_OFF, header, HFS_VH_LEN);
    expect_hfs_invalid(fixture, "software-locked", "software-locked");

    make_hfs_fixture(fixture, 1u);
    header = fixture + HFS_VH_OFF;
    put_be32(header + 4u, 0u);
    memcpy(fixture + FIXTURE_SIZE - HFS_VH_OFF, header, HFS_VH_LEN);
    expect_hfs_invalid(fixture, "dirty-mounted", "not cleanly unmounted");

    make_hfs_fixture(fixture, 1u);
    header = fixture + HFS_VH_OFF;
    put_be32(header + 4u, (1u << 8) | (1u << 11));
    memcpy(fixture + FIXTURE_SIZE - HFS_VH_OFF, header, HFS_VH_LEN);
    expect_hfs_invalid(fixture, "boot-inconsistent", "boot-inconsistent");
}

static void make_overlapping_reserved_fixture(uint8_t image[2048]) {
    uint8_t *header;

    memset(image, 0, 2048u);
    header = image + HFS_VH_OFF;
    put_be16(header, 0x4858u);
    put_be16(header + 2u, 5u);
    put_be32(header + 4u, 1u << 8);
    put_be32(header + 40u, FIXTURE_BLOCK_SIZE);
    put_be32(header + 44u, 4u);
    put_be32(header + 48u, 0u);
    put_be32(header + 52u, 0u);
    put_be64(header + 112u, 1u);
    put_be32(header + 124u, 1u);
    put_be32(header + 128u, 3u);
    put_be32(header + 132u, 1u);
    image[3u * FIXTURE_BLOCK_SIZE] = 0xf0u; /* blocks 0..3 allocated */
    memcpy(image + FIXTURE_BLOCK_SIZE, FSTAB_STOCK,
           sizeof(FSTAB_STOCK) - 1u);
    /* At 2048 bytes, primary and alternate headers are the same 512 bytes. */
}

static void test_growth_rejects_head_tail_overlap(void) {
    char source[160];
    char destination[160];
    uint8_t fixture[2048];
    rootfs_work_options_t options;
    rootfs_work_result_t result;

    CHECK(make_path(source, sizeof(source), "overlap-source"),
          "could not form overlap source path");
    CHECK(make_path(destination, sizeof(destination), "overlap-work"),
          "could not form overlap destination path");
    remove_if_present(source);
    remove_if_present(destination);
    make_overlapping_reserved_fixture(fixture);
    CHECK(write_file(source, fixture, sizeof(fixture)),
          "could not write overlap fixture");
    memset(&options, 0, sizeof(options));
    options.growth_bytes = 4u * FIXTURE_BLOCK_SIZE;
    options.io_buffer_bytes = 13u;
    CHECK(rootfs_work_create(source, destination, &options, &result) ==
              ROOTFS_WORK_HFS_INVALID &&
          result.stage == ROOTFS_WORK_STAGE_GROW_PLAN,
          "head/tail overlap returned %s/%s: %s",
          rootfs_work_status_name(result.status),
          rootfs_work_stage_name(result.stage), result.detail);
    CHECK(strstr(result.detail, "head and tail") != NULL,
          "head/tail refusal detail was '%s'", result.detail);
    CHECK(!path_exists(destination) && no_temporary_files(),
          "head/tail overlap left output artifacts");
    remove_if_present(destination);
    remove_if_present(source);
}

static void test_existing_destination_preserved(void) {
    char source[160];
    char destination[160];
    uint8_t fixture[FIXTURE_SIZE];
    uint8_t sentinel[] = {0x51u, 0x62u, 0x73u, 0x84u};
    uint8_t observed[sizeof(sentinel)];
    rootfs_work_result_t result;

    CHECK(make_path(source, sizeof(source), "existing-source"),
          "could not form source path");
    CHECK(make_path(destination, sizeof(destination), "existing-destination"),
          "could not form destination path");
    remove_if_present(source);
    remove_if_present(destination);
    make_hfs_fixture(fixture, 1u);
    CHECK(write_file(source, fixture, sizeof(fixture)),
          "could not write source fixture");
    CHECK(write_file(destination, sentinel, sizeof(sentinel)),
          "could not write destination sentinel");
    CHECK(rootfs_work_create(source, destination, NULL, &result) ==
              ROOTFS_WORK_DESTINATION_EXISTS,
          "existing destination returned %s/%s",
          rootfs_work_status_name(result.status),
          rootfs_work_stage_name(result.stage));
    CHECK(read_file(destination, observed, sizeof(observed)) &&
          memcmp(sentinel, observed, sizeof(sentinel)) == 0 &&
          file_size(destination) == sizeof(sentinel),
          "existing destination was changed");
    CHECK(no_temporary_files(), "existing destination created a temp file");
    remove_if_present(destination);
    remove_if_present(source);
}

static bool make_hard_link(const char *existing, const char *alias) {
#ifdef _WIN32
    return CreateHardLinkA(alias, existing, NULL) != 0;
#else
    return link(existing, alias) == 0;
#endif
}

static void test_source_hardlink_refused(void) {
    char source[160];
    char alias[160];
    char destination[160];
    uint8_t fixture[FIXTURE_SIZE];
    rootfs_work_result_t result;

    CHECK(make_path(source, sizeof(source), "hardlink-source"),
          "could not form source path");
    CHECK(make_path(alias, sizeof(alias), "hardlink-alias"),
          "could not form alias path");
    CHECK(make_path(destination, sizeof(destination), "hardlink-work"),
          "could not form destination path");
    remove_if_present(source);
    remove_if_present(alias);
    remove_if_present(destination);
    make_hfs_fixture(fixture, 1u);
    CHECK(write_file(source, fixture, sizeof(fixture)),
          "could not write hard-link fixture");
    if (make_hard_link(source, alias)) {
        CHECK(rootfs_work_create(source, destination, NULL, &result) ==
                  ROOTFS_WORK_SOURCE_ALIAS,
              "hard-linked source returned %s/%s",
              rootfs_work_status_name(result.status),
              rootfs_work_stage_name(result.stage));
        CHECK(!path_exists(destination) && no_temporary_files(),
              "hard-linked source created output artifacts");
    } else {
        CHECK(false, "could not create hard-link fixture (error %d)", errno);
    }
    remove_if_present(destination);
    remove_if_present(alias);
    remove_if_present(source);
}

#ifndef _WIN32
static void test_symbolic_links_refused(void) {
    char source[160];
    char source_link[160];
    char destination[160];
    char destination_link[160];
    uint8_t fixture[FIXTURE_SIZE];
    rootfs_work_result_t result;

    CHECK(make_path(source, sizeof(source), "symlink-source"),
          "could not form source path");
    CHECK(make_path(source_link, sizeof(source_link), "symlink-source-link"),
          "could not form source-link path");
    CHECK(make_path(destination, sizeof(destination), "symlink-work"),
          "could not form destination path");
    CHECK(make_path(destination_link, sizeof(destination_link),
                    "symlink-destination"),
          "could not form destination-link path");
    remove_if_present(source);
    remove_if_present(source_link);
    remove_if_present(destination);
    remove_if_present(destination_link);
    make_hfs_fixture(fixture, 1u);
    CHECK(write_file(source, fixture, sizeof(fixture)),
          "could not write symlink fixture");
    CHECK(symlink(source, source_link) == 0,
          "could not create source symlink: %d", errno);
    CHECK(rootfs_work_create(source_link, destination, NULL, &result) ==
              ROOTFS_WORK_PATH_UNSAFE,
          "source symlink returned %s/%s",
          rootfs_work_status_name(result.status),
          rootfs_work_stage_name(result.stage));
    CHECK(!path_exists(destination), "source symlink published output");
    CHECK(symlink(source, destination_link) == 0,
          "could not create destination symlink: %d", errno);
    CHECK(rootfs_work_create(source, destination_link, NULL, &result) ==
              ROOTFS_WORK_PATH_UNSAFE,
          "destination symlink returned %s/%s",
          rootfs_work_status_name(result.status),
          rootfs_work_stage_name(result.stage));
    CHECK(no_temporary_files(), "symlink refusals created temp artifacts");
    remove_if_present(destination_link);
    remove_if_present(destination);
    remove_if_present(source_link);
    remove_if_present(source);
}
#endif

static void test_argument_and_growth_guards(void) {
    char source[160];
    char destination[160];
    uint8_t fixture[FIXTURE_SIZE];
    rootfs_work_options_t options;
    rootfs_work_result_t result;

    CHECK(rootfs_work_create("x", "y", NULL, NULL) ==
              ROOTFS_WORK_INVALID_ARGUMENT,
          "NULL result was accepted");
    memset(&options, 0, sizeof(options));
    options.io_buffer_bytes = ROOTFS_WORK_MAX_IO_BUFFER + 1u;
    CHECK(rootfs_work_create("x", "y", &options, &result) ==
              ROOTFS_WORK_INVALID_ARGUMENT &&
          result.stage == ROOTFS_WORK_STAGE_ARGUMENTS,
          "oversized I/O buffer was accepted");

    CHECK(make_path(source, sizeof(source), "tiny-grow"),
          "could not form source path");
    CHECK(make_path(destination, sizeof(destination), "tiny-grow-work"),
          "could not form destination path");
    remove_if_present(source);
    remove_if_present(destination);
    make_hfs_fixture(fixture, 1u);
    CHECK(write_file(source, fixture, sizeof(fixture)),
          "could not write tiny-growth fixture");
    memset(&options, 0, sizeof(options));
    options.growth_bytes = FIXTURE_BLOCK_SIZE;
    options.io_buffer_bytes = 31u;
    CHECK(rootfs_work_create(source, destination, &options, &result) ==
              ROOTFS_WORK_GROW_INVALID,
          "sub-minimum growth returned %s/%s: %s",
          rootfs_work_status_name(result.status),
          rootfs_work_stage_name(result.stage), result.detail);
    CHECK(!path_exists(destination) && no_temporary_files(),
          "rejected growth left output artifacts");
    remove_if_present(destination);
    remove_if_present(source);
}

int main(void) {
    printf("rootfs work-image provisioner tests\n");
    test_sha256_known_answers_and_chunking();
    test_sha256_invalid_and_overflow_guards();
    test_success_boundary_growth_and_source_immutable();
    test_required_identity_chunk_boundaries();
    test_source_identity_policy_and_cleanup();
    test_fstab_uniqueness_and_cleanup();
    test_malformed_hfs_refused();
    test_growth_rejects_head_tail_overlap();
    test_existing_destination_preserved();
    test_source_hardlink_refused();
#ifndef _WIN32
    test_symbolic_links_refused();
#endif
    test_argument_and_growth_guards();
    printf("\nrootfs-work: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

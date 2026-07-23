/*
 * Exact-build, full-measurement, overlap, and transactional tests for the
 * iPhone OS 3.1.3 (7E18) external-memory-disk kernel compatibility gate.
 */
#include "ios3_kernel_patch.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned g_passes;
static unsigned g_failures;

#define CHECK(condition, ...) do {                                           \
    if (condition) {                                                         \
        g_passes++;                                                          \
    } else {                                                                \
        g_failures++;                                                        \
        printf("  FAIL %s:%d: ", __func__, __LINE__);                       \
        printf(__VA_ARGS__);                                                 \
        printf("\n");                                                       \
    }                                                                        \
} while (0)

typedef struct {
    char name[17];
    uint32_t vmaddr;
    uint32_t vmsize;
    uint32_t fileoff;
    uint32_t filesize;
} expected_segment_t;

static const expected_segment_t expected_segments[] = {
    {"__TEXT",          0xc0008000u, 0x00205000u, 0x00000000u, 0x00205000u},
    {"__DATA",          0xc020d000u, 0x00053000u, 0x00205000u, 0x00018000u},
    {"__HIB",           0xc0000000u, 0x00005000u, 0x0021d000u, 0x00005000u},
    {"__KLD",           0xc0260000u, 0x00001000u, 0x00222000u, 0x00001000u},
    {"__LAST",          0xc0261000u, 0x00000000u, 0x00223000u, 0x00000000u},
    {"__PRELINK_TEXT",  0xc02cd000u, 0x004c8000u, 0x0028f000u, 0x004c8000u},
    {"__PRELINK_STATE", 0xc0261000u, 0x00000000u, 0x00223000u, 0x00000000u},
    {"__PRELINK_INFO",  0xc0795000u, 0x0003c000u, 0x00757000u, 0x0003c000u},
    {"__LINKEDIT",      0xc0261000u, 0x0006b5a4u, 0x00223000u, 0x0006b5a4u}
};

typedef struct {
    uint32_t va;
    uint32_t length;
    uint8_t expected[4];
    uint8_t replacement[4];
    uint32_t site;
} expected_site_t;

static const uint8_t expected_watcher[] = {
    0xf0u, 0xb5u, 0x46u, 0x46u
};

static const uint8_t expected_uiomove[] = {
    0xf0u, 0xb5u, 0x5eu, 0x46u
};

static const expected_site_t expected_sites[] = {
    {
        IOS3_KERNEL_PATCH_IORTC_VA, 2u,
        {0x1eu, 0x23u, 0u, 0u}, {0x00u, 0x23u, 0u, 0u},
        IOS3_KERNEL_PATCH_SITE_IORTC
    },
    {
        IOS3_KERNEL_PATCH_BSD_ROOT_VA, 2u,
        {0x00u, 0x23u, 0u, 0u}, {0x01u, 0x23u, 0u, 0u},
        IOS3_KERNEL_PATCH_SITE_BSD_ROOT
    },
    {
        IOS3_KERNEL_PATCH_MD_READ_VA, 4u,
        {0xefu, 0xf7u, 0x12u, 0xf9u},
        {0xe1u, 0xdfu, 0xc0u, 0x46u},
        IOS3_KERNEL_PATCH_SITE_MD_READ
    },
    {
        IOS3_KERNEL_PATCH_MD_WRITE_VA, 4u,
        {0xefu, 0xf7u, 0xbfu, 0xf8u},
        {0xe2u, 0xdfu, 0xc0u, 0x46u},
        IOS3_KERNEL_PATCH_SITE_MD_WRITE
    },
    {
        IOS3_KERNEL_PATCH_RAW_WATCHER_VA, 4u,
        {0xf0u, 0xb5u, 0x46u, 0x46u},
        {0xe3u, 0xdfu, 0xe4u, 0xdfu},
        IOS3_KERNEL_PATCH_SITE_RAW_WATCHER
    }
};

/* A union's alignment is sufficient for every member.  Using the two API
 * structures as members keeps these byte fixtures suitably aligned without
 * depending on max_align_t, which is absent in some supported MSVC C modes. */
typedef union {
    ios3_kernel_patch_request_t request;
    ios3_kernel_patch_report_t report;
} patch_api_alignment_t;

/* BSS-backed fixtures keep heap use at zero while still presenting the exact
 * production-sized immutable file and complete file-backed RAM span. */
typedef union {
    patch_api_alignment_t alignment;
    uint8_t bytes[0x007d1000u];
} aligned_kernel_storage_t;

typedef union {
    patch_api_alignment_t alignment;
    uint8_t bytes[0x007d1000u];
} aligned_ram_storage_t;

static aligned_kernel_storage_t g_kernel_storage;
static aligned_ram_storage_t g_ram_storage;
static size_t g_segment_commands[9];
static size_t g_uuid_command;
static size_t g_thread_command;
static size_t g_symtab_command;

typedef struct {
    uint8_t *ram;
    ios3_kernel_patch_request_t request;
} fixture_t;

static void write_le32(uint8_t *bytes, size_t offset, uint32_t value) {
    bytes[offset] = (uint8_t)value;
    bytes[offset + 1u] = (uint8_t)(value >> 8);
    bytes[offset + 2u] = (uint8_t)(value >> 16);
    bytes[offset + 3u] = (uint8_t)(value >> 24);
}

static size_t ram_offset_for_va(uint32_t va) {
    return (size_t)(va - IOS3_KERNEL_PATCH_VIRT_BASE);
}

static size_t text_file_offset_for_va(uint32_t va) {
    return (size_t)(va - UINT32_C(0xc0008000));
}

static void build_synthetic_kernel(void) {
    uint8_t *kernel = g_kernel_storage.bytes;
    size_t offset = 28u;
    size_t segment_index;
    size_t site_index;

    memset(kernel, 0, sizeof g_kernel_storage.bytes);
    write_le32(kernel, 0u, MH_MAGIC_32);
    write_le32(kernel, 4u, MH_CPU_TYPE_ARM);
    write_le32(kernel, 8u, MH_CPU_SUBTYPE_V6);
    write_le32(kernel, 12u, MH_EXECUTE);

    for (segment_index = 0u;
         segment_index < sizeof expected_segments / sizeof expected_segments[0];
         segment_index++) {
        const expected_segment_t *segment = &expected_segments[segment_index];
        g_segment_commands[segment_index] = offset;
        write_le32(kernel, offset, LC_SEGMENT);
        write_le32(kernel, offset + 4u, 56u);
        memcpy(kernel + offset + 8u, segment->name, 16u);
        write_le32(kernel, offset + 24u, segment->vmaddr);
        write_le32(kernel, offset + 28u, segment->vmsize);
        write_le32(kernel, offset + 32u, segment->fileoff);
        write_le32(kernel, offset + 36u, segment->filesize);
        write_le32(kernel, offset + 48u, 0u);
        offset += 56u;
    }

    g_uuid_command = offset;
    write_le32(kernel, offset, LC_UUID);
    write_le32(kernel, offset + 4u, 24u);
    memcpy(kernel + offset + 8u, ios3_kernel_patch_expected_uuid,
           IOS3_KERNEL_PATCH_UUID_LENGTH);
    offset += 24u;

    g_thread_command = offset;
    write_le32(kernel, offset, LC_UNIXTHREAD);
    write_le32(kernel, offset + 4u, 84u);
    write_le32(kernel, offset + 12u, 17u);
    write_le32(kernel, offset + 16u + 13u * 4u, 0u);
    write_le32(kernel, offset + 16u + 15u * 4u,
               UINT32_C(0xc0069040));
    offset += 84u;

    g_symtab_command = offset;
    write_le32(kernel, offset, LC_SYMTAB);
    write_le32(kernel, offset + 4u, 24u);
    write_le32(kernel, offset + 8u, UINT32_C(0x00223000));
    write_le32(kernel, offset + 12u, UINT32_C(11431));
    write_le32(kernel, offset + 16u, UINT32_C(0x002447d4));
    write_le32(kernel, offset + 20u, UINT32_C(0x00049dd0));
    offset += 24u;

    write_le32(kernel, 16u, 12u);
    write_le32(kernel, 20u, (uint32_t)(offset - 28u));

    memcpy(kernel + text_file_offset_for_va(
               IOS3_KERNEL_PATCH_RAW_WATCHER_VA),
           expected_watcher, sizeof expected_watcher);
    memcpy(kernel + text_file_offset_for_va(IOS3_KERNEL_UIOMOVE_VA),
           expected_uiomove, sizeof expected_uiomove);
    for (site_index = 0u;
         site_index < sizeof expected_sites / sizeof expected_sites[0];
         site_index++) {
        memcpy(kernel + text_file_offset_for_va(expected_sites[site_index].va),
               expected_sites[site_index].expected,
               expected_sites[site_index].length);
    }
}

static void load_expected_segments(uint8_t *ram) {
    size_t segment_index;

    memset(ram, 0, sizeof g_ram_storage.bytes);
    for (segment_index = 0u;
         segment_index < sizeof expected_segments / sizeof expected_segments[0];
         segment_index++) {
        const expected_segment_t *segment = &expected_segments[segment_index];
        if (segment->filesize != 0u) {
            memcpy(ram + ram_offset_for_va(segment->vmaddr),
                   g_kernel_storage.bytes + segment->fileoff,
                   segment->filesize);
        }
    }
}

static void bind_fixture(fixture_t *fixture) {
    fixture->ram = g_ram_storage.bytes;
    load_expected_segments(fixture->ram);
    fixture->request = (ios3_kernel_patch_request_t){
        .kernel_file = g_kernel_storage.bytes,
        .kernel_file_size = (size_t)IOS3_KERNEL_PATCH_FILE_SIZE,
        .ram = fixture->ram,
        .ram_size = IOS3_KERNEL_PATCH_MIN_RAM_SIZE,
        .ram_base = IOS3_KERNEL_PATCH_RAM_BASE,
        .virt_base = IOS3_KERNEL_PATCH_VIRT_BASE
    };
}

static void prepare_synthetic_fixture(fixture_t *fixture) {
    build_synthetic_kernel();
    bind_fixture(fixture);
}

static bool bytes_are_zero(const uint8_t *bytes, size_t length) {
    size_t index;

    for (index = 0u; index < length; index++) {
        if (bytes[index] != 0u)
            return false;
    }
    return true;
}

static void check_loaded_segments_equal_file(const fixture_t *fixture,
                                             const char *description) {
    size_t segment_index;

    for (segment_index = 0u;
         segment_index < sizeof expected_segments / sizeof expected_segments[0];
         segment_index++) {
        const expected_segment_t *segment = &expected_segments[segment_index];
        const uint8_t *ram_bytes = fixture->ram +
                                   ram_offset_for_va(segment->vmaddr);

        CHECK(segment->filesize == 0u ||
                  memcmp(ram_bytes,
                         g_kernel_storage.bytes + segment->fileoff,
                         segment->filesize) == 0,
              "%s changed loaded segment %u", description,
              (unsigned)segment_index);
        CHECK(bytes_are_zero(ram_bytes + segment->filesize,
                             segment->vmsize - segment->filesize),
              "%s changed zero-fill tail of segment %u", description,
              (unsigned)segment_index);
    }
}

static void check_one_loaded_difference(
        const fixture_t *fixture,
        size_t target_segment,
        uint32_t target_byte,
        uint8_t target_value,
        const char *description) {
    size_t segment_index;

    for (segment_index = 0u;
         segment_index < sizeof expected_segments / sizeof expected_segments[0];
         segment_index++) {
        const expected_segment_t *segment = &expected_segments[segment_index];
        const uint8_t *file_bytes = g_kernel_storage.bytes + segment->fileoff;
        const uint8_t *ram_bytes = fixture->ram +
                                   ram_offset_for_va(segment->vmaddr);
        if (segment_index != target_segment) {
            CHECK(segment->filesize == 0u ||
                      memcmp(ram_bytes, file_bytes, segment->filesize) == 0,
                   "%s changed unrelated segment %u", description,
                   (unsigned)segment_index);
            CHECK(bytes_are_zero(ram_bytes + segment->filesize,
                                 segment->vmsize - segment->filesize),
                  "%s changed unrelated zero-fill segment %u", description,
                  (unsigned)segment_index);
        } else if (target_byte < segment->filesize) {
            CHECK(memcmp(ram_bytes, file_bytes, target_byte) == 0,
                   "%s changed bytes before the target", description);
            CHECK(ram_bytes[target_byte] == target_value,
                  "%s changed the mismatching byte", description);
            CHECK(memcmp(ram_bytes + target_byte + 1u,
                         file_bytes + target_byte + 1u,
                          segment->filesize - target_byte - 1u) == 0,
                   "%s changed bytes after the target", description);
            CHECK(bytes_are_zero(ram_bytes + segment->filesize,
                                 segment->vmsize - segment->filesize),
                  "%s changed the target segment zero-fill tail",
                  description);
        } else {
            CHECK(segment->filesize == 0u ||
                      memcmp(ram_bytes, file_bytes, segment->filesize) == 0,
                  "%s changed the target segment file bytes", description);
            CHECK(bytes_are_zero(ram_bytes + segment->filesize,
                                 target_byte - segment->filesize),
                  "%s changed zero-fill bytes before the target",
                  description);
            CHECK(ram_bytes[target_byte] == target_value,
                  "%s changed the zero-fill mismatching byte", description);
            CHECK(bytes_are_zero(ram_bytes + target_byte + 1u,
                                 segment->vmsize - target_byte - 1u),
                  "%s changed zero-fill bytes after the target",
                  description);
        }
    }
}

static void expect_failure_unchanged(
        fixture_t *fixture,
        ios3_kernel_patch_status_t expected_status,
        const char *description) {
    ios3_kernel_patch_report_t report;
    ios3_kernel_patch_status_t status =
        ios3_kernel_patch_apply(&fixture->request, &report);

    CHECK(status == expected_status && report.status == expected_status,
          "%s returned %s", description,
          ios3_kernel_patch_status_string(status));
    check_loaded_segments_equal_file(fixture, description);
}

static void test_manifest_constants(void) {
    static const uint8_t uuid[IOS3_KERNEL_PATCH_UUID_LENGTH] = {
        0x7fu, 0x87u, 0xddu, 0x4bu, 0xdcu, 0x3du, 0xf5u, 0x22u,
        0xa8u, 0x43u, 0x07u, 0x57u, 0x08u, 0x59u, 0x35u, 0x7eu
    };
    static const uint8_t digest[IOS3_KERNEL_PATCH_SHA256_LENGTH] = {
        0x0du, 0x8cu, 0xdbu, 0x33u, 0x9du, 0x37u, 0xcfu, 0x37u,
        0xa1u, 0xdbu, 0x26u, 0x38u, 0xffu, 0xf7u, 0x92u, 0x72u,
        0xecu, 0xd6u, 0x3au, 0x17u, 0x76u, 0x4bu, 0xf7u, 0x66u,
        0x6eu, 0xfau, 0x16u, 0x18u, 0x72u, 0x5du, 0xf7u, 0x0cu
    };

    CHECK(IOS3_KERNEL_PATCH_FILE_SIZE == UINT64_C(7942144),
          "kernel size constant drifted");
    CHECK(IOS3_KERNEL_PATCH_MIN_RAM_SIZE == (size_t)UINT32_C(0x007d1000),
          "full loaded-segment RAM bound drifted");
    CHECK(IOS3_KERNEL_PATCH_VIRT_BASE == UINT32_C(0xc0000000) &&
          IOS3_KERNEL_PATCH_RAM_BASE == UINT64_C(0x08000000),
          "kernel VA-to-PA mapping constants drifted");
    CHECK(IOS3_KERNEL_PATCH_RAW_WATCHER_VA == UINT32_C(0xc0073f94) &&
          IOS3_KERNEL_UIOMOVE_VA == UINT32_C(0xc0128d14) &&
          IOS3_KERNEL_PATCH_IORTC_VA == UINT32_C(0xc0175b3e) &&
          IOS3_KERNEL_PATCH_BSD_ROOT_VA == UINT32_C(0xc01a1b5a) &&
          IOS3_KERNEL_PATCH_MD_READ_VA == UINT32_C(0xc0074140) &&
          IOS3_KERNEL_PATCH_MD_WRITE_VA == UINT32_C(0xc00741e6),
          "one or more exact kernel site addresses drifted");
    CHECK(memcmp(uuid, ios3_kernel_patch_expected_uuid, sizeof uuid) == 0,
          "raw LC_UUID constant drifted");
    CHECK(memcmp(digest, ios3_kernel_patch_expected_sha256,
                 sizeof digest) == 0,
          "kernel SHA-256 constant drifted");
}

static void test_sha256_known_answers(void) {
    static const uint8_t empty_digest[32] = {
        0xe3u, 0xb0u, 0xc4u, 0x42u, 0x98u, 0xfcu, 0x1cu, 0x14u,
        0x9au, 0xfbu, 0xf4u, 0xc8u, 0x99u, 0x6fu, 0xb9u, 0x24u,
        0x27u, 0xaeu, 0x41u, 0xe4u, 0x64u, 0x9bu, 0x93u, 0x4cu,
        0xa4u, 0x95u, 0x99u, 0x1bu, 0x78u, 0x52u, 0xb8u, 0x55u
    };
    static const uint8_t abc_digest[32] = {
        0xbau, 0x78u, 0x16u, 0xbfu, 0x8fu, 0x01u, 0xcfu, 0xeau,
        0x41u, 0x41u, 0x40u, 0xdeu, 0x5du, 0xaeu, 0x22u, 0x23u,
        0xb0u, 0x03u, 0x61u, 0xa3u, 0x96u, 0x17u, 0x7au, 0x9cu,
        0xb4u, 0x10u, 0xffu, 0x61u, 0xf2u, 0x00u, 0x15u, 0xadu
    };
    static const char two_block_message[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    static const uint8_t two_block_digest[32] = {
        0x24u, 0x8du, 0x6au, 0x61u, 0xd2u, 0x06u, 0x38u, 0xb8u,
        0xe5u, 0xc0u, 0x26u, 0x93u, 0x0cu, 0x3eu, 0x60u, 0x39u,
        0xa3u, 0x3cu, 0xe4u, 0x59u, 0x64u, 0xffu, 0x21u, 0x67u,
        0xf6u, 0xecu, 0xedu, 0xd4u, 0x19u, 0xdbu, 0x06u, 0xc1u
    };
    uint8_t digest[32];
    uint8_t alias[64] = {0};

    CHECK(ios3_kernel_patch_sha256(NULL, 0u, digest) &&
          memcmp(digest, empty_digest, sizeof digest) == 0,
          "empty SHA-256 known-answer test failed");
    CHECK(ios3_kernel_patch_sha256((const uint8_t *)"abc", 3u, digest) &&
          memcmp(digest, abc_digest, sizeof digest) == 0,
          "abc SHA-256 known-answer test failed");
    CHECK(ios3_kernel_patch_sha256(
              (const uint8_t *)two_block_message,
              sizeof two_block_message - 1u, digest) &&
          memcmp(digest, two_block_digest, sizeof digest) == 0,
          "two-block-padding SHA-256 known-answer test failed");

    memcpy(alias, "abc", 3u);
    CHECK(ios3_kernel_patch_sha256(alias, 3u, alias) &&
          memcmp(alias, abc_digest, sizeof abc_digest) == 0,
          "overlapping SHA-256 output was not delayed until after input");
    memset(digest, 0xa5, sizeof digest);
    CHECK(!ios3_kernel_patch_sha256(NULL, 1u, digest) &&
          digest[0] == 0xa5u,
          "NULL nonempty SHA-256 input changed output");
    CHECK(!ios3_kernel_patch_sha256((const uint8_t *)"abc", 3u, NULL),
          "NULL SHA-256 output was accepted");
}

static void test_identity_and_topology_fail_closed(fixture_t *fixture) {
    ios3_kernel_patch_report_t report;
    size_t byte_index;

    prepare_synthetic_fixture(fixture);
    fixture->request.kernel_file_size--;
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_KERNEL_SIZE_MISMATCH, "wrong file size");

    build_synthetic_kernel();
    g_kernel_storage.bytes[0] ^= 0xffu;
    bind_fixture(fixture);
    CHECK(ios3_kernel_patch_apply(&fixture->request, &report) ==
              IOS3_KERNEL_PATCH_STATUS_MACHO_PARSE_FAILED &&
          report.macho_status == MACHO_ERR_BAD_MAGIC,
          "bad Mach-O magic lost parser detail");
    check_loaded_segments_equal_file(fixture, "bad Mach-O magic");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes, 4u, MH_CPU_TYPE_ARM + 1u);
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_CPU_TYPE_MISMATCH, "wrong CPU type");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes, 8u, MH_CPU_SUBTYPE_V6 + 1u);
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_CPU_SUBTYPE_MISMATCH, "wrong CPU subtype");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes, 12u, MH_EXECUTE + 1u);
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_FILE_TYPE_MISMATCH, "wrong file type");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes, g_uuid_command, UINT32_C(0x99));
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_UUID_MISSING, "absent LC_UUID");

    for (byte_index = 0u; byte_index < IOS3_KERNEL_PATCH_UUID_LENGTH;
         byte_index++) {
        build_synthetic_kernel();
        g_kernel_storage.bytes[g_uuid_command + 8u + byte_index] ^= 0xffu;
        bind_fixture(fixture);
        CHECK(ios3_kernel_patch_apply(&fixture->request, &report) ==
                  IOS3_KERNEL_PATCH_STATUS_UUID_MISMATCH &&
              report.byte_index == byte_index,
              "UUID byte %u mismatch was accepted", (unsigned)byte_index);
        check_loaded_segments_equal_file(fixture, "UUID mismatch");
    }

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes, g_segment_commands[0],
               UINT32_C(0x99));
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_SEGMENT_COUNT_MISMATCH,
        "missing segment command");

    build_synthetic_kernel();
    g_kernel_storage.bytes[g_segment_commands[0] + 8u] ^= 1u;
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_SEGMENT_NAME_MISMATCH,
        "wrong segment name");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes, g_segment_commands[0] + 24u,
               UINT32_C(0xc0009000));
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_SEGMENT_VMADDR_MISMATCH,
        "wrong segment vmaddr");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes, g_segment_commands[0] + 28u,
               UINT32_C(0x00206000));
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_SEGMENT_VMSIZE_MISMATCH,
        "wrong segment vmsize");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes, g_segment_commands[0] + 32u, 1u);
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_SEGMENT_FILEOFF_MISMATCH,
        "wrong segment file offset");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes, g_segment_commands[0] + 36u,
               UINT32_C(0x00204fff));
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_SEGMENT_FILESIZE_MISMATCH,
        "wrong segment file size");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes, g_thread_command, UINT32_C(0x99));
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_ENTRYPOINT_MISSING,
        "missing entrypoint command");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes,
               g_thread_command + 16u + 15u * 4u,
               UINT32_C(0xc0069042));
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_ENTRYPOINT_MISMATCH,
        "wrong entrypoint");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes,
               g_thread_command + 16u + 13u * 4u, 4u);
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_ENTRY_STACK_MISMATCH,
        "wrong initial stack");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes, g_symtab_command, UINT32_C(0x99));
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_SYMTAB_MISSING,
        "missing symbol table");

    build_synthetic_kernel();
    write_le32(g_kernel_storage.bytes, g_symtab_command + 12u,
               UINT32_C(11430));
    bind_fixture(fixture);
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_SYMTAB_MISMATCH,
        "wrong symbol table topology");
}

static void test_synthetic_metadata_cannot_assert_digest(fixture_t *fixture) {
    ios3_kernel_patch_report_t report;

    prepare_synthetic_fixture(fixture);
    CHECK(ios3_kernel_patch_apply(&fixture->request, &report) ==
              IOS3_KERNEL_PATCH_STATUS_KERNEL_DIGEST_MISMATCH,
          "synthetic exact metadata/topology bypassed full-file SHA-256");
    CHECK(report.byte_index < IOS3_KERNEL_PATCH_SHA256_LENGTH &&
          report.expected_value != report.actual_value,
          "digest mismatch report lost the first differing byte");
    check_loaded_segments_equal_file(fixture, "synthetic digest mismatch");
}

static void test_geometry_and_nulls(fixture_t *fixture) {
    ios3_kernel_patch_report_t report;

    prepare_synthetic_fixture(fixture);
    CHECK(ios3_kernel_patch_apply(NULL, &report) ==
              IOS3_KERNEL_PATCH_STATUS_INVALID_ARGUMENT &&
          report.status == IOS3_KERNEL_PATCH_STATUS_INVALID_ARGUMENT,
          "NULL request was not reported");
    CHECK(ios3_kernel_patch_apply(&fixture->request, NULL) ==
              IOS3_KERNEL_PATCH_STATUS_INVALID_ARGUMENT,
          "NULL report was accepted");

    prepare_synthetic_fixture(fixture);
    fixture->request.kernel_file = NULL;
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_INVALID_ARGUMENT, "NULL kernel pointer");

    prepare_synthetic_fixture(fixture);
    fixture->request.ram = NULL;
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_INVALID_ARGUMENT, "NULL RAM pointer");

    prepare_synthetic_fixture(fixture);
    fixture->request.virt_base++;
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_VIRT_BASE_MISMATCH, "wrong virtual base");

    prepare_synthetic_fixture(fixture);
    fixture->request.ram_base++;
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_RAM_BASE_MISMATCH, "wrong physical base");

    prepare_synthetic_fixture(fixture);
    fixture->request.ram_size = 0u;
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_INVALID_RAM_GEOMETRY,
        "empty RAM aperture");

    prepare_synthetic_fixture(fixture);
    fixture->request.ram_size = IOS3_KERNEL_PATCH_MIN_RAM_SIZE - 1u;
    expect_failure_unchanged(fixture,
        IOS3_KERNEL_PATCH_STATUS_RAM_TOO_SMALL,
        "RAM aperture missing the final loaded segment byte");
}

static void test_overlap_rules_precede_report_writes(fixture_t *fixture) {
    union {
        patch_api_alignment_t alignment;
        uint8_t bytes[sizeof(ios3_kernel_patch_report_t) >
                      sizeof(ios3_kernel_patch_request_t)
                          ? sizeof(ios3_kernel_patch_report_t)
                          : sizeof(ios3_kernel_patch_request_t)];
    } alias;
    union {
        patch_api_alignment_t alignment;
        uint8_t bytes[64u + sizeof(ios3_kernel_patch_report_t)];
    } adjacent;
    ios3_kernel_patch_request_t request;
    ios3_kernel_patch_report_t report;
    uint8_t before_alias[sizeof alias.bytes];
    uint8_t before_report[sizeof report];
    ios3_kernel_patch_status_t status;

    prepare_synthetic_fixture(fixture);
    memset(&alias, 0, sizeof alias);
    *(ios3_kernel_patch_request_t *)(void *)alias.bytes = fixture->request;
    memcpy(before_alias, alias.bytes, sizeof before_alias);
    status = ios3_kernel_patch_apply(
        (const ios3_kernel_patch_request_t *)(const void *)alias.bytes,
        (ios3_kernel_patch_report_t *)(void *)alias.bytes);
    CHECK(status == IOS3_KERNEL_PATCH_STATUS_REPORT_OVERLAP &&
          memcmp(alias.bytes, before_alias, sizeof before_alias) == 0,
          "report/request overlap wrote through the aliased report");

    prepare_synthetic_fixture(fixture);
    memset(g_kernel_storage.bytes, 0xa5, sizeof report);
    memcpy(before_report, g_kernel_storage.bytes, sizeof before_report);
    status = ios3_kernel_patch_apply(
        &fixture->request,
        (ios3_kernel_patch_report_t *)(void *)g_kernel_storage.bytes);
    CHECK(status == IOS3_KERNEL_PATCH_STATUS_REPORT_OVERLAP &&
          memcmp(g_kernel_storage.bytes, before_report,
                 sizeof before_report) == 0,
          "report/kernel overlap changed immutable kernel bytes");

    prepare_synthetic_fixture(fixture);
    memset(fixture->ram, 0xa5, sizeof report);
    memcpy(before_report, fixture->ram, sizeof before_report);
    status = ios3_kernel_patch_apply(
        &fixture->request,
        (ios3_kernel_patch_report_t *)(void *)fixture->ram);
    CHECK(status == IOS3_KERNEL_PATCH_STATUS_REPORT_OVERLAP &&
          memcmp(fixture->ram, before_report, sizeof before_report) == 0,
          "report/RAM overlap changed guest bytes");

    prepare_synthetic_fixture(fixture);
    *(ios3_kernel_patch_request_t *)(void *)g_kernel_storage.bytes =
        fixture->request;
    memcpy(before_alias, g_kernel_storage.bytes,
           sizeof(ios3_kernel_patch_request_t));
    status = ios3_kernel_patch_apply(
        (const ios3_kernel_patch_request_t *)(const void *)
            g_kernel_storage.bytes,
        &report);
    CHECK(status == IOS3_KERNEL_PATCH_STATUS_REQUEST_KERNEL_OVERLAP &&
          report.status == status &&
          memcmp(g_kernel_storage.bytes, before_alias,
                 sizeof(ios3_kernel_patch_request_t)) == 0,
          "request/kernel overlap was not fail-closed");

    prepare_synthetic_fixture(fixture);
    *(ios3_kernel_patch_request_t *)(void *)fixture->ram = fixture->request;
    memcpy(before_alias, fixture->ram,
           sizeof(ios3_kernel_patch_request_t));
    status = ios3_kernel_patch_apply(
        (const ios3_kernel_patch_request_t *)(const void *)fixture->ram,
        &report);
    CHECK(status == IOS3_KERNEL_PATCH_STATUS_REQUEST_RAM_OVERLAP &&
          report.status == status &&
          memcmp(fixture->ram, before_alias,
                 sizeof(ios3_kernel_patch_request_t)) == 0,
          "request/RAM overlap was not fail-closed");

    prepare_synthetic_fixture(fixture);
    request = fixture->request;
    request.ram = g_kernel_storage.bytes;
    request.ram_size = IOS3_KERNEL_PATCH_MIN_RAM_SIZE;
    status = ios3_kernel_patch_apply(&request, &report);
    CHECK(status == IOS3_KERNEL_PATCH_STATUS_KERNEL_RAM_OVERLAP &&
          report.status == status,
          "kernel/RAM overlap was not rejected");

    prepare_synthetic_fixture(fixture);
    request = fixture->request;
    request.kernel_file_size = SIZE_MAX;
    memset(&report, 0xa5, sizeof report);
    memcpy(before_report, &report, sizeof before_report);
    status = ios3_kernel_patch_apply(&request, &report);
    CHECK(status == IOS3_KERNEL_PATCH_STATUS_HOST_RANGE_OVERFLOW &&
          memcmp(&report, before_report, sizeof before_report) == 0,
          "host range overflow wrote a report before proving disjointness");

    prepare_synthetic_fixture(fixture);
    memset(&adjacent, 0, sizeof adjacent);
    request = fixture->request;
    request.kernel_file = adjacent.bytes;
    request.kernel_file_size = 64u;
    status = ios3_kernel_patch_apply(
        &request,
        (ios3_kernel_patch_report_t *)(void *)(adjacent.bytes + 64u));
    CHECK(status == IOS3_KERNEL_PATCH_STATUS_KERNEL_SIZE_MISMATCH,
          "adjacent exclusive-end report was treated as overlapping");
}

static void test_loaded_segment_relationship(fixture_t *fixture) {
    size_t segment_index;

    for (segment_index = 0u;
         segment_index < sizeof expected_segments / sizeof expected_segments[0];
         segment_index++) {
        const expected_segment_t *segment = &expected_segments[segment_index];
        uint32_t positions[3];
        size_t position_index;

        if (segment->filesize == 0u)
            continue;
        positions[0] = 0u;
        positions[1] = segment->filesize / 2u;
        positions[2] = segment->filesize - 1u;

        for (position_index = 0u; position_index < 3u; position_index++) {
            uint32_t segment_byte = positions[position_index];
            size_t ram_offset = ram_offset_for_va(segment->vmaddr) +
                                segment_byte;
            uint8_t expected;
            uint8_t corrupted;
            ios3_kernel_patch_report_t report;

            prepare_synthetic_fixture(fixture);
            expected = fixture->ram[ram_offset];
            fixture->ram[ram_offset] ^= 0x5au;
            corrupted = fixture->ram[ram_offset];
            CHECK(ios3_kernel_patch_apply(&fixture->request, &report) ==
                      IOS3_KERNEL_PATCH_STATUS_LOADED_SEGMENT_MISMATCH,
                  "segment %u position %u mismatch was accepted",
                  (unsigned)segment_index, (unsigned)position_index);
            CHECK(report.segment_index == segment_index &&
                  report.byte_index == segment_byte &&
                  report.virtual_address ==
                      (uint64_t)segment->vmaddr + segment_byte &&
                  report.physical_address == IOS3_KERNEL_PATCH_RAM_BASE +
                      ram_offset_for_va(segment->vmaddr) + segment_byte &&
                  report.expected_value == expected &&
                  report.actual_value == corrupted,
                  "segment %u position %u report lost detail",
                  (unsigned)segment_index, (unsigned)position_index);
            check_one_loaded_difference(
                fixture, segment_index, segment_byte, corrupted,
                "loaded-segment mismatch rejection");
        }

        if (segment->vmsize > segment->filesize) {
            uint32_t tail_length = segment->vmsize - segment->filesize;
            uint32_t tail_positions[3] = {
                segment->filesize,
                segment->filesize + tail_length / 2u,
                segment->vmsize - 1u
            };

            for (position_index = 0u; position_index < 3u;
                 position_index++) {
                uint32_t segment_byte = tail_positions[position_index];
                size_t ram_offset = ram_offset_for_va(segment->vmaddr) +
                                    segment_byte;
                ios3_kernel_patch_report_t report;

                prepare_synthetic_fixture(fixture);
                fixture->ram[ram_offset] = (uint8_t)(0x41u + position_index);
                CHECK(ios3_kernel_patch_apply(&fixture->request, &report) ==
                          IOS3_KERNEL_PATCH_STATUS_LOADED_SEGMENT_MISMATCH,
                      "segment %u zero-fill position %u was accepted",
                      (unsigned)segment_index, (unsigned)position_index);
                CHECK(report.segment_index == segment_index &&
                      report.byte_index == segment_byte &&
                      report.virtual_address ==
                          (uint64_t)segment->vmaddr + segment_byte &&
                      report.physical_address == IOS3_KERNEL_PATCH_RAM_BASE +
                          ram_offset_for_va(segment->vmaddr) + segment_byte &&
                      report.expected_value == 0u &&
                      report.actual_value == fixture->ram[ram_offset],
                      "segment %u zero-fill position %u report lost detail",
                      (unsigned)segment_index, (unsigned)position_index);
                check_one_loaded_difference(
                    fixture, segment_index, segment_byte,
                    fixture->ram[ram_offset],
                    "loaded zero-fill mismatch rejection");
            }
        }
    }
}

static void test_raw_and_patch_site_mismatches(fixture_t *fixture) {
    size_t byte_index;
    size_t site_index;

    for (byte_index = 0u; byte_index < sizeof expected_watcher; byte_index++) {
        size_t offset = ram_offset_for_va(
            IOS3_KERNEL_PATCH_RAW_WATCHER_VA) + byte_index;
        uint8_t corrupted;
        ios3_kernel_patch_report_t report;

        prepare_synthetic_fixture(fixture);
        fixture->ram[offset] ^= 0xffu;
        corrupted = fixture->ram[offset];
        CHECK(ios3_kernel_patch_apply(&fixture->request, &report) ==
                  IOS3_KERNEL_PATCH_STATUS_RAW_WATCHER_MISMATCH &&
              report.site == IOS3_KERNEL_PATCH_SITE_RAW_WATCHER &&
              report.byte_index == byte_index,
              "raw watcher byte %u mismatch lost its site report",
              (unsigned)byte_index);
        CHECK(fixture->ram[offset] == corrupted,
              "raw watcher mismatch changed RAM");
    }

    for (byte_index = 0u; byte_index < sizeof expected_uiomove; byte_index++) {
        size_t offset =
            ram_offset_for_va(IOS3_KERNEL_UIOMOVE_VA) + byte_index;
        uint8_t corrupted;
        ios3_kernel_patch_report_t report;

        prepare_synthetic_fixture(fixture);
        fixture->ram[offset] ^= 0xffu;
        corrupted = fixture->ram[offset];
        CHECK(ios3_kernel_patch_apply(&fixture->request, &report) ==
                  IOS3_KERNEL_PATCH_STATUS_UIOMOVE_MISMATCH &&
              report.site == IOS3_KERNEL_PATCH_NO_SITE &&
              report.byte_index == byte_index &&
              report.virtual_address ==
                  (uint64_t)IOS3_KERNEL_UIOMOVE_VA + byte_index &&
              report.physical_address ==
                  IOS3_KERNEL_PATCH_RAM_BASE + offset &&
              report.expected_value == expected_uiomove[byte_index] &&
              report.actual_value == corrupted,
              "uiomove target byte %u mismatch lost its exact report",
              (unsigned)byte_index);
        CHECK(fixture->ram[offset] == corrupted,
              "uiomove target mismatch changed RAM");
    }

    for (site_index = 0u;
         site_index < sizeof expected_sites / sizeof expected_sites[0];
         site_index++) {
        const expected_site_t *site = &expected_sites[site_index];
        /* The raw entry has a deliberately more precise pre-transaction
         * diagnostic, exercised byte-for-byte by the loop above. */
        if (site->site == IOS3_KERNEL_PATCH_SITE_RAW_WATCHER)
            continue;
        for (byte_index = 0u; byte_index < site->length; byte_index++) {
            size_t offset = ram_offset_for_va(site->va) + byte_index;
            uint8_t corrupted;
            ios3_kernel_patch_report_t report;

            prepare_synthetic_fixture(fixture);
            fixture->ram[offset] ^= 0x80u;
            corrupted = fixture->ram[offset];
            CHECK(ios3_kernel_patch_apply(&fixture->request, &report) ==
                      IOS3_KERNEL_PATCH_STATUS_PATCH_TRANSACTION_FAILED &&
                  report.site == site->site &&
                  report.byte_index == byte_index &&
                  report.guest_patch_status ==
                      GUEST_PATCH_STATUS_EXPECTED_MISMATCH,
                  "patch site %u byte %u mismatch lost detail",
                  (unsigned)site_index, (unsigned)byte_index);
            CHECK(fixture->ram[offset] == corrupted,
                  "patch site mismatch changed RAM");
        }
    }
}

static FILE *open_read_only(const char *path) {
#ifdef _MSC_VER
    FILE *file = NULL;
    if (fopen_s(&file, path, "rb") != 0)
        return NULL;
    return file;
#else
    return fopen(path, "rb");
#endif
}

static bool load_private_kernel(const char *path) {
    FILE *file = open_read_only(path);
    size_t got;
    int trailing;

    if (file == NULL)
        return false;
    memset(g_kernel_storage.bytes, 0, sizeof g_kernel_storage.bytes);
    got = fread(g_kernel_storage.bytes, 1u,
                (size_t)IOS3_KERNEL_PATCH_FILE_SIZE, file);
    trailing = fgetc(file);
    if (fclose(file) != 0)
        return false;
    return got == (size_t)IOS3_KERNEL_PATCH_FILE_SIZE && trailing == EOF;
}

static void test_private_kernel_positive(fixture_t *fixture,
                                         const char *kernel_path) {
    uint8_t digest_before[IOS3_KERNEL_PATCH_SHA256_LENGTH];
    uint8_t digest_after[IOS3_KERNEL_PATCH_SHA256_LENGTH];
    ios3_kernel_patch_report_t report;
    size_t site_index;

    CHECK(load_private_kernel(kernel_path),
          "could not read exact private kernel fixture %s", kernel_path);
    if (g_failures != 0u)
        return;
    bind_fixture(fixture);

    CHECK(ios3_kernel_patch_sha256(
              g_kernel_storage.bytes,
              (size_t)IOS3_KERNEL_PATCH_FILE_SIZE, digest_before) &&
          memcmp(digest_before, ios3_kernel_patch_expected_sha256,
                 sizeof digest_before) == 0,
          "private kernel did not match the pinned SHA-256");
    CHECK(memcmp(g_kernel_storage.bytes + text_file_offset_for_va(
                     IOS3_KERNEL_PATCH_RAW_WATCHER_VA),
                 expected_watcher, sizeof expected_watcher) == 0,
          "private kernel raw watcher bytes drifted");
    CHECK(memcmp(g_kernel_storage.bytes +
                     text_file_offset_for_va(IOS3_KERNEL_UIOMOVE_VA),
                 expected_uiomove, sizeof expected_uiomove) == 0,
          "private kernel uiomove target bytes drifted");
    for (site_index = 0u;
         site_index < sizeof expected_sites / sizeof expected_sites[0];
         site_index++) {
        const expected_site_t *site = &expected_sites[site_index];
        CHECK(memcmp(g_kernel_storage.bytes +
                         text_file_offset_for_va(site->va),
                     site->expected, site->length) == 0,
              "private kernel original bytes drifted at site %u",
              (unsigned)site_index);
    }

    /* Mutate both immutable source and its mapped RAM counterpart so topology
     * and the file-to-RAM relationship still pass; only full SHA-256 may stop
     * this altered build. The final file byte belongs to __PRELINK_INFO. */
    {
        const expected_segment_t *segment = &expected_segments[7];
        uint32_t segment_byte = segment->filesize - 1u;
        size_t file_offset = (size_t)segment->fileoff + segment_byte;
        size_t ram_offset = ram_offset_for_va(segment->vmaddr) + segment_byte;
        uint8_t original = g_kernel_storage.bytes[file_offset];

        g_kernel_storage.bytes[file_offset] ^= 1u;
        fixture->ram[ram_offset] ^= 1u;
        CHECK(ios3_kernel_patch_apply(&fixture->request, &report) ==
                  IOS3_KERNEL_PATCH_STATUS_KERNEL_DIGEST_MISMATCH,
              "full-file mutation with matching RAM bypassed SHA-256");
        CHECK(g_kernel_storage.bytes[file_offset] == (uint8_t)(original ^ 1u) &&
              fixture->ram[ram_offset] == (uint8_t)(original ^ 1u),
              "digest rejection changed source or RAM");
        g_kernel_storage.bytes[file_offset] = original;
        fixture->ram[ram_offset] = original;
    }

    memset(&report, 0xa5, sizeof report);
    CHECK(ios3_kernel_patch_apply(&fixture->request, &report) ==
              IOS3_KERNEL_PATCH_STATUS_OK,
          "exact private kernel and bootkernel-style RAM did not patch");
    CHECK(report.status == IOS3_KERNEL_PATCH_STATUS_OK &&
          report.site == IOS3_KERNEL_PATCH_NO_SITE &&
          report.segment_index == IOS3_KERNEL_PATCH_NO_SEGMENT &&
          report.byte_index == IOS3_KERNEL_PATCH_NO_BYTE &&
          report.macho_status == MACHO_OK &&
          report.guest_patch_status == GUEST_PATCH_STATUS_OK,
          "private-kernel success report retained stale detail");
    for (site_index = 0u;
         site_index < sizeof expected_sites / sizeof expected_sites[0];
         site_index++) {
        const expected_site_t *site = &expected_sites[site_index];
        CHECK(memcmp(fixture->ram + ram_offset_for_va(site->va),
                     site->replacement, site->length) == 0,
              "successful private manifest wrote wrong bytes at site %u",
              (unsigned)site_index);
    }

    CHECK(ios3_kernel_patch_sha256(
              g_kernel_storage.bytes,
              (size_t)IOS3_KERNEL_PATCH_FILE_SIZE, digest_after) &&
          memcmp(digest_before, digest_after, sizeof digest_before) == 0,
          "successful patch modified immutable kernel-file bytes");
    CHECK(ios3_kernel_patch_apply(&fixture->request, &report) ==
              IOS3_KERNEL_PATCH_STATUS_PATCH_TRANSACTION_FAILED &&
          report.site == IOS3_KERNEL_PATCH_SITE_IORTC &&
          report.guest_patch_status == GUEST_PATCH_STATUS_EXPECTED_MISMATCH,
          "second application did not fail on the full IORTC instruction");
}

static void test_status_and_site_strings(void) {
    int status;
    uint32_t site;

    for (status = IOS3_KERNEL_PATCH_STATUS_OK;
         status <= IOS3_KERNEL_PATCH_STATUS_PATCH_TRANSACTION_FAILED;
         status++) {
        const char *message = ios3_kernel_patch_status_string(
            (ios3_kernel_patch_status_t)status);
        CHECK(message != NULL && message[0] != '\0',
              "status %d has no stable string", status);
    }
    CHECK(strcmp(ios3_kernel_patch_status_string(
                     (ios3_kernel_patch_status_t)999),
                 "unknown iOS 3 kernel patch status") == 0,
          "unknown status string changed");
    for (site = IOS3_KERNEL_PATCH_SITE_IORTC;
         site <= IOS3_KERNEL_PATCH_SITE_RAW_WATCHER; site++) {
        const char *name = ios3_kernel_patch_site_string(site);
        CHECK(name != NULL && name[0] != '\0',
              "site %u has no stable name", (unsigned)site);
    }
    CHECK(strcmp(ios3_kernel_patch_site_string(IOS3_KERNEL_PATCH_NO_SITE),
                 "none") == 0,
          "no-site sentinel has no stable name");
    CHECK(strcmp(ios3_kernel_patch_site_string(999u),
                 "unknown kernel patch site") == 0,
          "unknown site string changed");
}

int main(int argc, char **argv) {
    fixture_t fixture;

    test_manifest_constants();
    test_sha256_known_answers();
    test_identity_and_topology_fail_closed(&fixture);
    test_synthetic_metadata_cannot_assert_digest(&fixture);
    test_geometry_and_nulls(&fixture);
    test_overlap_rules_precede_report_writes(&fixture);
    test_loaded_segment_relationship(&fixture);
    test_raw_and_patch_site_mismatches(&fixture);
    test_status_and_site_strings();

    if (argc == 2) {
        test_private_kernel_positive(&fixture, argv[1]);
    } else if (argc != 1) {
        printf("usage: %s [private-kernel.macho]\n", argv[0]);
        g_failures++;
    } else {
        printf("ios3_kernel_patch: private exact-positive fixture not supplied; "
               "public negative/KAT suite only\n");
    }

    printf("ios3_kernel_patch: %u passed, %u failed\n",
           g_passes, g_failures);
    return g_failures == 0u ? 0 : 1;
}

/*
 * iOS3-VM -- adversarial tests for atomic guest-RAM patch manifests.
 *
 * These fixtures are entirely synthetic.  Firmware-specific addresses and
 * patch bytes belong in the boot frontend, never this generic primitive.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "guest_patch.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_RAM_SIZE 64u
#define TEST_VIRT_BASE UINT32_C(0xc0000000)
#define TEST_RAM_BASE UINT64_C(0x10000000)

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

static guest_patch_manifest_t make_manifest(
        uint8_t *ram, size_t ram_size, uint64_t ram_base, uint32_t virt_base,
        const guest_patch_entry_t *entries, size_t entry_count) {
    return (guest_patch_manifest_t){
        .ram = ram,
        .ram_size = ram_size,
        .ram_base = ram_base,
        .virt_base = virt_base,
        .entries = entries,
        .entry_count = entry_count
    };
}

static void test_supported_lengths_and_mapping(void) {
    uint8_t ram[TEST_RAM_SIZE] = {0};
    guest_patch_entry_t entries[] = {
        { .virtual_address = TEST_VIRT_BASE + 1u, .length = 1u,
          .expected = {0x11u}, .replacement = {0xa1u} },
        { .virtual_address = TEST_VIRT_BASE + 4u, .length = 2u,
          .expected = {0x21u, 0x22u}, .replacement = {0xb1u, 0xb2u} },
        { .virtual_address = TEST_VIRT_BASE + 8u, .length = 4u,
          .expected = {0x31u, 0x32u, 0x33u, 0x34u},
          .replacement = {0xc1u, 0xc2u, 0xc3u, 0xc4u} },
        { .virtual_address = TEST_VIRT_BASE + 16u, .length = 8u,
          .expected = {0x41u, 0x42u, 0x43u, 0x44u,
                       0x45u, 0x46u, 0x47u, 0x48u},
          .replacement = {0xd1u, 0xd2u, 0xd3u, 0xd4u,
                          0xd5u, 0xd6u, 0xd7u, 0xd8u} }
    };
    guest_patch_report_t report;
    guest_patch_manifest_t manifest;
    size_t i;

    for (i = 0u; i < sizeof(entries) / sizeof(entries[0]); i++) {
        uint32_t byte_index;
        size_t offset = entries[i].virtual_address - TEST_VIRT_BASE;
        for (byte_index = 0u; byte_index < entries[i].length; byte_index++)
            ram[offset + byte_index] = entries[i].expected[byte_index];
    }
    manifest = make_manifest(ram, sizeof(ram), TEST_RAM_BASE, TEST_VIRT_BASE,
                             entries, sizeof(entries) / sizeof(entries[0]));

    CHECK(guest_patch_apply(&manifest, &report) == GUEST_PATCH_STATUS_OK,
          "1/2/4/8-byte manifest should apply");
    CHECK(report.status == GUEST_PATCH_STATUS_OK &&
          report.entry_index == GUEST_PATCH_NO_INDEX &&
          report.byte_index == GUEST_PATCH_NO_INDEX &&
          report.virtual_address == GUEST_PATCH_NO_ADDRESS &&
          report.physical_address == GUEST_PATCH_NO_ADDRESS,
          "success report was not completely reset");
    for (i = 0u; i < sizeof(entries) / sizeof(entries[0]); i++) {
        uint32_t byte_index;
        size_t offset = entries[i].virtual_address - TEST_VIRT_BASE;
        for (byte_index = 0u; byte_index < entries[i].length; byte_index++) {
            CHECK(ram[offset + byte_index] == entries[i].replacement[byte_index],
                  "entry %u byte %u was not replaced", (unsigned)i,
                  (unsigned)byte_index);
        }
    }
}

static void test_adjacent_and_maximum_count(void) {
    uint8_t ram[TEST_RAM_SIZE] = {0};
    guest_patch_entry_t entries[GUEST_PATCH_MAX_ENTRIES];
    guest_patch_report_t report;
    guest_patch_manifest_t manifest;
    uint32_t i;

    for (i = 0u; i < GUEST_PATCH_MAX_ENTRIES; i++) {
        entries[i] = (guest_patch_entry_t){
            .virtual_address = TEST_VIRT_BASE + i,
            .length = 1u,
            .expected = {0u},
            .replacement = {(uint8_t)(i + 1u)}
        };
    }
    manifest = make_manifest(ram, sizeof(ram), TEST_RAM_BASE, TEST_VIRT_BASE,
                             entries, GUEST_PATCH_MAX_ENTRIES);

    CHECK(guest_patch_apply(&manifest, &report) == GUEST_PATCH_STATUS_OK,
          "64 adjacent entries should be accepted");
    for (i = 0u; i < GUEST_PATCH_MAX_ENTRIES; i++) {
        CHECK(ram[i] == (uint8_t)(i + 1u),
              "adjacent entry %u was not applied", (unsigned)i);
    }
}

static void test_overlap_and_duplicate_are_atomic(void) {
    uint8_t ram[TEST_RAM_SIZE] = {0};
    uint8_t before[TEST_RAM_SIZE];
    guest_patch_entry_t overlap[] = {
        { .virtual_address = TEST_VIRT_BASE + 10u, .length = 4u,
          .expected = {0u, 0u, 0u, 0u},
          .replacement = {1u, 2u, 3u, 4u} },
        { .virtual_address = TEST_VIRT_BASE + 12u, .length = 2u,
          .expected = {0u, 0u}, .replacement = {5u, 6u} }
    };
    guest_patch_entry_t duplicate[] = {
        { .virtual_address = TEST_VIRT_BASE + 20u, .length = 1u,
          .expected = {0u}, .replacement = {1u} },
        { .virtual_address = TEST_VIRT_BASE + 20u, .length = 1u,
          .expected = {0u}, .replacement = {2u} }
    };
    guest_patch_report_t report;
    guest_patch_manifest_t manifest;

    memcpy(before, ram, sizeof(ram));
    manifest = make_manifest(ram, sizeof(ram), TEST_RAM_BASE, TEST_VIRT_BASE,
                             overlap, 2u);
    CHECK(guest_patch_apply(&manifest, &report) == GUEST_PATCH_STATUS_OVERLAP,
          "partially overlapping entries were accepted");
    CHECK(report.entry_index == 1u && report.byte_index == 0u &&
          report.virtual_address == (uint64_t)TEST_VIRT_BASE + 12u &&
          report.physical_address == TEST_RAM_BASE + 12u,
          "overlap report did not identify the first shared byte");
    CHECK(memcmp(ram, before, sizeof(ram)) == 0,
          "overlap rejection modified RAM");

    manifest.entries = duplicate;
    CHECK(guest_patch_apply(&manifest, &report) == GUEST_PATCH_STATUS_OVERLAP,
          "duplicate entry was accepted");
    CHECK(report.entry_index == 1u && report.byte_index == 0u,
          "duplicate report did not identify the second entry");
    CHECK(memcmp(ram, before, sizeof(ram)) == 0,
          "duplicate rejection modified RAM");
}

static void test_boundaries_and_arithmetic(void) {
    uint8_t ram[TEST_RAM_SIZE] = {0};
    guest_patch_entry_t entry = {
        .virtual_address = TEST_VIRT_BASE + TEST_RAM_SIZE - 1u,
        .length = 1u,
        .expected = {0u},
        .replacement = {0x5au}
    };
    guest_patch_report_t report;
    guest_patch_manifest_t manifest = make_manifest(
        ram, sizeof(ram), TEST_RAM_BASE, TEST_VIRT_BASE, &entry, 1u);

    CHECK(guest_patch_apply(&manifest, &report) == GUEST_PATCH_STATUS_OK &&
          ram[TEST_RAM_SIZE - 1u] == 0x5au,
          "last byte of RAM should be patchable");

    entry = (guest_patch_entry_t){
        .virtual_address = TEST_VIRT_BASE - 1u, .length = 1u,
        .expected = {0u}, .replacement = {1u}
    };
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_VA_BELOW_BASE,
          "VA below virt_base was accepted");
    CHECK(report.entry_index == 0u && report.byte_index == 0u &&
          report.virtual_address == TEST_VIRT_BASE - 1u &&
          report.physical_address == GUEST_PATCH_NO_ADDRESS,
          "below-base report is imprecise");

    entry = (guest_patch_entry_t){
        .virtual_address = UINT32_MAX, .length = 2u,
        .expected = {0u, 0u}, .replacement = {1u, 2u}
    };
    manifest.virt_base = 0u;
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_VA_OVERFLOW,
          "entry crossing the 32-bit VA end was accepted");
    CHECK(report.byte_index == 1u &&
          report.virtual_address == UINT64_C(0x100000000),
          "VA overflow report did not name the first unrepresentable byte");

    entry = (guest_patch_entry_t){
        .virtual_address = 16u, .length = 1u,
        .expected = {0u}, .replacement = {1u}
    };
    manifest = make_manifest(ram, 16u, UINT64_C(0xfffffff0), 0u, &entry, 1u);
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_PA_OVERFLOW,
          "entry crossing the 32-bit physical end was accepted");
    CHECK(report.byte_index == 0u && report.virtual_address == 16u &&
          report.physical_address == UINT64_C(0x100000000),
          "physical overflow report is imprecise");
}

static void test_out_of_ram(void) {
    uint8_t ram[TEST_RAM_SIZE] = {0};
    guest_patch_entry_t entry = {
        .virtual_address = TEST_VIRT_BASE + TEST_RAM_SIZE,
        .length = 1u, .expected = {0u}, .replacement = {1u}
    };
    guest_patch_report_t report;
    guest_patch_manifest_t manifest = make_manifest(
        ram, sizeof(ram), TEST_RAM_BASE, TEST_VIRT_BASE, &entry, 1u);

    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_OUT_OF_RAM,
          "entry starting at exclusive RAM end was accepted");
    CHECK(report.byte_index == 0u &&
          report.virtual_address == (uint64_t)TEST_VIRT_BASE + TEST_RAM_SIZE &&
          report.physical_address == TEST_RAM_BASE + TEST_RAM_SIZE,
          "out-of-RAM start report is imprecise");

    entry.virtual_address--;
    entry.length = 2u;
    entry.expected[1] = 0u;
    entry.replacement[1] = 2u;
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_OUT_OF_RAM,
          "entry straddling the RAM end was accepted");
    CHECK(report.byte_index == 1u &&
          report.virtual_address == (uint64_t)TEST_VIRT_BASE + TEST_RAM_SIZE,
          "straddling report did not identify its first outside byte");
}

static void test_geometry_count_length_and_noop(void) {
    uint8_t ram[TEST_RAM_SIZE] = {0};
    guest_patch_entry_t entry = {
        .virtual_address = TEST_VIRT_BASE, .length = 1u,
        .expected = {0u}, .replacement = {1u}
    };
    guest_patch_report_t report;
    guest_patch_manifest_t manifest = make_manifest(
        ram, sizeof(ram), TEST_RAM_BASE, TEST_VIRT_BASE, &entry, 1u);

    manifest.ram_size = 0u;
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_INVALID_GEOMETRY,
          "empty RAM geometry was accepted");
    manifest.ram_size = sizeof(ram);
    manifest.ram_base = UINT64_C(0x100000000);
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_INVALID_GEOMETRY,
          "RAM base outside 32-bit aperture was accepted");
    manifest.ram_base = UINT64_C(0xffffffff);
    manifest.ram_size = 2u;
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_INVALID_GEOMETRY,
          "RAM aperture overflow was accepted");

    manifest.ram_base = TEST_RAM_BASE;
    manifest.ram_size = sizeof(ram);
    manifest.entry_count = 0u;
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_INVALID_COUNT,
          "empty manifest was accepted");
    manifest.entry_count = GUEST_PATCH_MAX_ENTRIES + 1u;
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_INVALID_COUNT,
          "manifest over the fixed maximum was accepted");

    manifest.entry_count = 1u;
    entry.length = 0u;
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_INVALID_LENGTH,
          "zero-length patch was accepted");
    entry.length = GUEST_PATCH_MAX_BYTES + 1u;
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_INVALID_LENGTH,
          "oversized patch was accepted");

    entry.length = 1u;
    entry.expected[0] = 0x33u;
    entry.replacement[0] = 0x33u;
    entry.expected[1] = 0x11u;
    entry.replacement[1] = 0x22u;
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_NO_CHANGE,
          "no-op over active bytes was accepted");
    CHECK(report.entry_index == 0u && report.byte_index == 0u &&
          report.expected == 0x33u && report.actual == 0x33u,
          "no-op report is imprecise");
}

static void test_null_arguments(void) {
    uint8_t ram[TEST_RAM_SIZE] = {0};
    guest_patch_entry_t entry = {
        .virtual_address = TEST_VIRT_BASE, .length = 1u,
        .expected = {0u}, .replacement = {1u}
    };
    guest_patch_report_t report;
    guest_patch_manifest_t manifest = make_manifest(
        ram, sizeof(ram), TEST_RAM_BASE, TEST_VIRT_BASE, &entry, 1u);

    CHECK(guest_patch_apply(NULL, &report) ==
              GUEST_PATCH_STATUS_INVALID_ARGUMENT &&
          report.status == GUEST_PATCH_STATUS_INVALID_ARGUMENT,
          "NULL manifest was not rejected and reported");
    CHECK(guest_patch_apply(&manifest, NULL) ==
              GUEST_PATCH_STATUS_INVALID_ARGUMENT,
          "NULL report was accepted");
    manifest.ram = NULL;
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_INVALID_ARGUMENT,
          "NULL RAM was accepted");
    manifest.ram = ram;
    manifest.entries = NULL;
    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_INVALID_ARGUMENT,
          "NULL entries were accepted");
}

static void test_late_mismatch_is_atomic_and_reported(void) {
    uint8_t ram[TEST_RAM_SIZE] = {0};
    uint8_t before[TEST_RAM_SIZE];
    guest_patch_entry_t entries[] = {
        { .virtual_address = TEST_VIRT_BASE + 2u, .length = 2u,
          .expected = {0x10u, 0x11u}, .replacement = {0xa0u, 0xa1u} },
        { .virtual_address = TEST_VIRT_BASE + 40u, .length = 4u,
          .expected = {0x20u, 0x21u, 0x22u, 0x23u},
          .replacement = {0xb0u, 0xb1u, 0xb2u, 0xb3u} }
    };
    guest_patch_report_t report;
    guest_patch_manifest_t manifest;

    ram[2] = 0x10u;
    ram[3] = 0x11u;
    ram[40] = 0x20u;
    ram[41] = 0x21u;
    ram[42] = 0xeeu; /* mismatch deliberately late in the manifest */
    ram[43] = 0x23u;
    memcpy(before, ram, sizeof(ram));
    manifest = make_manifest(ram, sizeof(ram), TEST_RAM_BASE, TEST_VIRT_BASE,
                             entries, 2u);

    CHECK(guest_patch_apply(&manifest, &report) ==
              GUEST_PATCH_STATUS_EXPECTED_MISMATCH,
          "late expected-byte mismatch was accepted");
    CHECK(memcmp(ram, before, sizeof(ram)) == 0,
          "late mismatch changed an earlier valid entry");
    CHECK(report.entry_index == 1u && report.byte_index == 2u &&
          report.virtual_address == (uint64_t)TEST_VIRT_BASE + 42u &&
          report.physical_address == TEST_RAM_BASE + 42u &&
          report.expected == 0x22u && report.actual == 0xeeu,
          "mismatch report lost entry/byte/address/value detail");
}

static void test_exact_top_of_physical_aperture(void) {
    uint8_t ram[1] = {0x44u};
    guest_patch_entry_t entry = {
        .virtual_address = TEST_VIRT_BASE, .length = 1u,
        .expected = {0x44u}, .replacement = {0x55u}
    };
    guest_patch_report_t report;
    guest_patch_manifest_t manifest = make_manifest(
        ram, sizeof(ram), UINT64_C(0xffffffff), TEST_VIRT_BASE, &entry, 1u);

    CHECK(guest_patch_apply(&manifest, &report) == GUEST_PATCH_STATUS_OK &&
          ram[0] == 0x55u,
          "exclusive physical end 0x100000000 should be valid");
}

static void test_manifest_storage_may_alias_ram(void) {
    /*
     * The first patch deliberately overwrites the second entry's source VA.
     * A one-pass implementation would then read a corrupted second entry;
     * building the complete private plan first makes this well-defined.
     */
    union {
        guest_patch_entry_t entries[2];
        uint8_t bytes[sizeof(guest_patch_entry_t) * 2u + 16u];
    } storage = {0};
    const size_t second_va_offset = sizeof(guest_patch_entry_t) +
                                    offsetof(guest_patch_entry_t,
                                             virtual_address);
    const size_t tail_offset = sizeof(guest_patch_entry_t) * 2u + 4u;
    uint8_t original_va_byte;
    guest_patch_report_t report;
    guest_patch_manifest_t manifest;

    storage.entries[0] = (guest_patch_entry_t){
        .virtual_address = TEST_VIRT_BASE + (uint32_t)second_va_offset,
        .length = 1u
    };
    storage.entries[1] = (guest_patch_entry_t){
        .virtual_address = TEST_VIRT_BASE + (uint32_t)tail_offset,
        .length = 1u,
        .expected = {0u},
        .replacement = {0x7cu}
    };
    original_va_byte = storage.bytes[second_va_offset];
    storage.entries[0].expected[0] = original_va_byte;
    storage.entries[0].replacement[0] = (uint8_t)(original_va_byte ^ 0xffu);

    manifest = make_manifest(storage.bytes, sizeof(storage.bytes),
                             TEST_RAM_BASE, TEST_VIRT_BASE,
                             storage.entries, 2u);
    CHECK(guest_patch_apply(&manifest, &report) == GUEST_PATCH_STATUS_OK,
          "manifest aliasing RAM corrupted a later plan entry");
    CHECK(storage.bytes[second_va_offset] ==
              (uint8_t)(original_va_byte ^ 0xffu) &&
          storage.bytes[tail_offset] == 0x7cu,
          "aliased manifest patches were not both applied");
}

static void test_manifest_object_may_patch_itself(void) {
    union {
        guest_patch_manifest_t manifest;
        uint8_t bytes[sizeof(guest_patch_manifest_t) + 16u];
    } storage = {0};
    guest_patch_entry_t entries[2] = {0};
    guest_patch_report_t report;
    size_t original_count = 2u;
    size_t replacement_count = 1u;
    const size_t count_offset = offsetof(guest_patch_manifest_t, entry_count);
    const size_t target_offset = sizeof(guest_patch_manifest_t) + 4u;

    storage.manifest = make_manifest(storage.bytes, sizeof(storage.bytes),
                                     TEST_RAM_BASE, TEST_VIRT_BASE,
                                     entries, original_count);

    entries[0].virtual_address = TEST_VIRT_BASE + (uint32_t)count_offset;
    entries[0].length = (uint32_t)sizeof(size_t);
    memcpy(entries[0].expected, &original_count, sizeof(original_count));
    memcpy(entries[0].replacement, &replacement_count,
           sizeof(replacement_count));
    entries[1] = (guest_patch_entry_t){
        .virtual_address = TEST_VIRT_BASE + (uint32_t)target_offset,
        .length = 1u,
        .expected = {0u},
        .replacement = {0xa5u}
    };

    CHECK(sizeof(size_t) <= GUEST_PATCH_MAX_BYTES,
          "size_t cannot fit in a bounded patch entry");
    CHECK(guest_patch_apply(&storage.manifest, &report) ==
              GUEST_PATCH_STATUS_OK,
          "self-modifying manifest object was rejected");
    CHECK(storage.manifest.entry_count == replacement_count,
          "first patch did not change the manifest's own entry count");
    CHECK(storage.bytes[target_offset] == 0xa5u,
          "self-modified entry count skipped the second planned patch");
}

static void test_status_strings(void) {
    int status;

    for (status = GUEST_PATCH_STATUS_OK;
         status <= GUEST_PATCH_STATUS_VERIFY_MISMATCH; status++) {
        const char *message = guest_patch_status_string(
            (guest_patch_status_t)status);
        CHECK(message != NULL && message[0] != '\0',
              "status %d has no stable message", status);
    }
    CHECK(strcmp(guest_patch_status_string((guest_patch_status_t)999),
                 "unknown guest patch status") == 0,
          "unknown status message changed");
}

int main(void) {
    test_supported_lengths_and_mapping();
    test_adjacent_and_maximum_count();
    test_overlap_and_duplicate_are_atomic();
    test_boundaries_and_arithmetic();
    test_out_of_ram();
    test_geometry_count_length_and_noop();
    test_null_arguments();
    test_late_mismatch_is_atomic_and_reported();
    test_exact_top_of_physical_aperture();
    test_manifest_storage_may_alias_ram();
    test_manifest_object_may_patch_itself();
    test_status_strings();

    printf("guest_patch: %u passed, %u failed\n", g_passes, g_failures);
    return g_failures == 0u ? 0 : 1;
}

/*
 * iOS3-VM -- adversarial tests for the XNU32 raw memory-disk bridge.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "md_raw_bridge.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_RAM_SIZE UINT32_C(131072)
#define TEST_MEDIA_SIZE UINT32_C(131072)
#define TEST_L1 UINT32_C(0x0001c000)
#define TEST_RAW_PC UINT32_C(0x00003000)
#define TEST_RAW_SVC UINT32_C(0x0000dfe3)
#define TEST_DEVICE UINT32_C(0x09000000)
#define TEST_USER_LIMIT UINT32_C(0xc0000000)
#define TEST_UIO UINT32_C(0x00001000)
#define TEST_IOV UINT32_C(0x00001100)
#define TEST_USER UINT32_C(0x00004000)

#define UIO_IOVS UINT32_C(0x00)
#define UIO_IOVCNT UINT32_C(0x04)
#define UIO_OFFSET_LO UINT32_C(0x08)
#define UIO_OFFSET_HI UINT32_C(0x0c)
#define UIO_SEGFLG UINT32_C(0x10)
#define UIO_RW UINT32_C(0x14)
#define UIO_RESID UINT32_C(0x18)
#define UIO_SIZE UINT32_C(0x1c)
#define UIO_MAX_IOVS UINT32_C(0x20)
#define UIO_FLAGS UINT32_C(0x24)

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
    uint8_t bytes[TEST_RAM_SIZE];
    uint64_t read32_calls;
} fake_ram_t;

static bool ram_resolve(uint32_t address, size_t length, size_t *offset) {
    uint64_t end = (uint64_t)address + length;
    if (end > TEST_RAM_SIZE)
        return false;
    *offset = address;
    return true;
}

static uint8_t ram_read8(void *context, uint32_t address) {
    fake_ram_t *ram = (fake_ram_t *)context;
    size_t offset;
    return ram_resolve(address, 1u, &offset) ? ram->bytes[offset] : 0u;
}

static uint16_t ram_read16(void *context, uint32_t address) {
    uint16_t value = ram_read8(context, address);
    value |= (uint16_t)((uint16_t)ram_read8(context, address + 1u) << 8);
    return value;
}

static uint32_t ram_read32(void *context, uint32_t address) {
    fake_ram_t *ram = (fake_ram_t *)context;
    size_t offset;
    uint32_t value;
    ram->read32_calls++;
    if (!ram_resolve(address, 4u, &offset))
        return 0u;
    value = ram->bytes[offset];
    value |= (uint32_t)ram->bytes[offset + 1u] << 8;
    value |= (uint32_t)ram->bytes[offset + 2u] << 16;
    value |= (uint32_t)ram->bytes[offset + 3u] << 24;
    return value;
}

static void ram_write8(void *context, uint32_t address, uint8_t value) {
    fake_ram_t *ram = (fake_ram_t *)context;
    size_t offset;
    if (ram_resolve(address, 1u, &offset))
        ram->bytes[offset] = value;
}

static void ram_write16(void *context, uint32_t address, uint16_t value) {
    ram_write8(context, address, (uint8_t)value);
    ram_write8(context, address + 1u, (uint8_t)(value >> 8));
}

static void ram_write32(void *context, uint32_t address, uint32_t value) {
    ram_write8(context, address, (uint8_t)value);
    ram_write8(context, address + 1u, (uint8_t)(value >> 8));
    ram_write8(context, address + 2u, (uint8_t)(value >> 16));
    ram_write8(context, address + 3u, (uint8_t)(value >> 24));
}

typedef enum {
    IO_NORMAL = 0,
    IO_ERROR,
    IO_PARTIAL_ERROR,
    IO_ALWAYS_ZERO,
    IO_OVERREPORT
} io_mode_t;

typedef struct {
    io_mode_t mode;
    unsigned calls;
    size_t first_chunk;
} io_behavior_t;

typedef struct {
    uint8_t bytes[TEST_MEDIA_SIZE];
    io_behavior_t read;
    io_behavior_t write;
} fake_block_t;

static vm_block_io_status_t fake_read_at(void *context, uint64_t offset,
                                         void *destination, size_t requested,
                                         size_t *actual) {
    fake_block_t *block = (fake_block_t *)context;
    size_t available;
    size_t count;
    block->read.calls++;
    *actual = 0u;
    if (block->read.mode == IO_ERROR ||
        (block->read.mode == IO_PARTIAL_ERROR && block->read.calls > 1u))
        return VM_BLOCK_IO_ERROR;
    if (block->read.mode == IO_ALWAYS_ZERO)
        return VM_BLOCK_IO_OK;
    if (block->read.mode == IO_OVERREPORT) {
        *actual = requested + 1u;
        return VM_BLOCK_IO_OK;
    }
    if (offset >= TEST_MEDIA_SIZE)
        return VM_BLOCK_IO_OK;
    available = TEST_MEDIA_SIZE - (size_t)offset;
    count = requested < available ? requested : available;
    if (block->read.mode == IO_PARTIAL_ERROR && block->read.calls == 1u &&
        block->read.first_chunk != 0u && count > block->read.first_chunk)
        count = block->read.first_chunk;
    memcpy(destination, block->bytes + (size_t)offset, count);
    *actual = count;
    return VM_BLOCK_IO_OK;
}

static vm_block_io_status_t fake_write_at(void *context, uint64_t offset,
                                          const void *source, size_t requested,
                                          size_t *actual) {
    fake_block_t *block = (fake_block_t *)context;
    size_t available;
    size_t count;
    block->write.calls++;
    *actual = 0u;
    if (block->write.mode == IO_ERROR ||
        (block->write.mode == IO_PARTIAL_ERROR && block->write.calls > 1u))
        return VM_BLOCK_IO_ERROR;
    if (block->write.mode == IO_ALWAYS_ZERO)
        return VM_BLOCK_IO_OK;
    if (block->write.mode == IO_OVERREPORT) {
        *actual = requested + 1u;
        return VM_BLOCK_IO_OK;
    }
    if (offset >= TEST_MEDIA_SIZE)
        return VM_BLOCK_IO_OK;
    available = TEST_MEDIA_SIZE - (size_t)offset;
    count = requested < available ? requested : available;
    if (block->write.mode == IO_PARTIAL_ERROR && block->write.calls == 1u &&
        block->write.first_chunk != 0u && count > block->write.first_chunk)
        count = block->write.first_chunk;
    memcpy(block->bytes + (size_t)offset, source, count);
    *actual = count;
    return VM_BLOCK_IO_OK;
}

static vm_block_io_status_t fake_flush(void *context) {
    (void)context;
    return VM_BLOCK_IO_OK;
}

typedef struct {
    fake_ram_t ram;
    arm_bus_t bus;
    fake_block_t fake_block;
    vm_block_t block;
    md_raw_bridge_t bridge;
    arm_cpu_t cpu;
} fixture_t;

/* Static because md_raw_bridge intentionally owns a 128 KiB staging area. */
static fixture_t fixture;

static void put_u32(uint32_t address, uint32_t value) {
    ram_write32(&fixture.ram, address, value);
}

static uint32_t get_u32(uint32_t address) {
    return ram_read32(&fixture.ram, address);
}

static void put_uio_at(uint32_t address, uint32_t iov, int32_t iovcnt,
                       int64_t offset, uint32_t segment, uint32_t rw,
                       int32_t residual, int32_t max_iovs) {
    put_u32(address + UIO_IOVS, iov);
    put_u32(address + UIO_IOVCNT, (uint32_t)iovcnt);
    put_u32(address + UIO_OFFSET_LO, (uint32_t)(uint64_t)offset);
    put_u32(address + UIO_OFFSET_HI, (uint32_t)((uint64_t)offset >> 32));
    put_u32(address + UIO_SEGFLG, segment);
    put_u32(address + UIO_RW, rw);
    put_u32(address + UIO_RESID, (uint32_t)residual);
    put_u32(address + UIO_SIZE,
            MD_RAW_BRIDGE_UIO_SIZE +
            (uint32_t)(max_iovs > 0 ? max_iovs : 0) *
                MD_RAW_BRIDGE_USER_IOV_SIZE);
    put_u32(address + UIO_MAX_IOVS, (uint32_t)max_iovs);
    put_u32(address + UIO_FLAGS, 1u);
}

static void put_uio(uint32_t iov, int32_t iovcnt, int64_t offset,
                    uint32_t segment, uint32_t rw, int32_t residual,
                    int32_t max_iovs) {
    put_uio_at(TEST_UIO, iov, iovcnt, offset, segment, rw, residual,
               max_iovs);
}

static void put_iov(uint32_t index, uint32_t base, uint32_t length) {
    uint32_t address = TEST_IOV + index * MD_RAW_BRIDGE_USER_IOV_SIZE;
    put_u32(address, base);
    put_u32(address + 4u, length);
}

static md_raw_bridge_config_t default_config(void) {
    md_raw_bridge_config_t config = {0};
    config.site.pc = TEST_RAW_PC;
    config.site.encoding = TEST_RAW_SVC;
    config.expected_device = TEST_DEVICE;
    config.user_address_limit = TEST_USER_LIMIT;
    config.media_size = TEST_MEDIA_SIZE;
    config.ram_base = 0u;
    config.ram_size = TEST_RAM_SIZE;
    config.ram = fixture.ram.bytes;
    config.block = &fixture.block;
    return config;
}

static void fixture_init(void) {
    md_raw_bridge_config_t config;
    size_t i;

    memset(&fixture, 0, sizeof fixture);
    fixture.bus.ctx = &fixture.ram;
    fixture.bus.read8 = ram_read8;
    fixture.bus.read16 = ram_read16;
    fixture.bus.read32 = ram_read32;
    fixture.bus.write8 = ram_write8;
    fixture.bus.write16 = ram_write16;
    fixture.bus.write32 = ram_write32;
    fixture.block.context = &fixture.fake_block;
    fixture.block.size = TEST_MEDIA_SIZE;
    fixture.block.identity = 1u;
    fixture.block.generation = 1u;
    fixture.block.read_at = fake_read_at;
    fixture.block.write_at = fake_write_at;
    fixture.block.flush = fake_flush;
    for (i = 0u; i < TEST_MEDIA_SIZE; i++)
        fixture.fake_block.bytes[i] = (uint8_t)(i ^ (i >> 8));

    config = default_config();
    md_raw_bridge_init(&fixture.bridge, &config);
    arm_reset(&fixture.cpu, &fixture.bus);
    fixture.cpu.cpsr = ARM_MODE_SVC | ARM_CPSR_T;
    fixture.cpu.r[0] = TEST_DEVICE;
    fixture.cpu.r[1] = TEST_UIO;
    fixture.cpu.r[14] = UINT32_C(0x00003501);

    /* One identity-mapped, manager-domain section. */
    put_u32(TEST_L1, UINT32_C(0x00000c02));
    fixture.cpu.cp15.ttbr0 = TEST_L1;
    fixture.cpu.cp15.ttbcr = 0u;
    fixture.cpu.cp15.dacr = 3u;
    fixture.cpu.cp15.sctlr |= ARM_SCTLR_M;

    put_uio(TEST_IOV, 1, 13, 5u, 0u, 32, 1);
    put_iov(0u, TEST_USER, 32u);
}

static arm_svc_result_t invoke(void) {
    return md_raw_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                                    TEST_RAW_PC, TEST_RAW_SVC);
}

static void expect_halt_error(md_raw_bridge_error_code_t code,
                              const char *label) {
    arm_svc_result_t result = invoke();
    CHECK(result == ARM_SVC_ERROR, "%s did not fail closed (%d)",
          label, (int)result);
    CHECK(fixture.bridge.last_error.code == code,
          "%s error %d expected %d", label,
          (int)fixture.bridge.last_error.code, (int)code);
}

static void test_read_write_and_exact_uio_commit(void) {
    arm_cpu_t before;
    uint8_t expected[32];
    size_t i;

    fixture_init();
    memset(fixture.ram.bytes + TEST_USER, 0xa5, sizeof expected);
    memcpy(expected, fixture.fake_block.bytes + 13u, sizeof expected);
    before = fixture.cpu;
    CHECK(invoke() == ARM_SVC_HANDLED, "valid raw read was not handled");
    CHECK(memcmp(fixture.ram.bytes + TEST_USER, expected,
                 sizeof expected) == 0, "raw read copied wrong bytes");
    CHECK(fixture.cpu.r[0] == 0u &&
          memcmp(&fixture.cpu.r[1], &before.r[1],
                 sizeof(before.r) - sizeof(before.r[0])) == 0 &&
          fixture.cpu.cpsr == before.cpsr,
          "raw read changed architectural state other than r0");
    CHECK(get_u32(TEST_IOV) == TEST_USER + sizeof expected &&
          get_u32(TEST_IOV + 4u) == 0u &&
          get_u32(TEST_UIO + UIO_IOVS) == TEST_IOV &&
          get_u32(TEST_UIO + UIO_IOVCNT) == 0u &&
          get_u32(TEST_UIO + UIO_OFFSET_LO) == 13u + sizeof expected &&
          get_u32(TEST_UIO + UIO_OFFSET_HI) == 0u &&
          get_u32(TEST_UIO + UIO_RESID) == 0u &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 5u,
          "raw read committed the wrong XNU uio state");
    CHECK(fixture.bridge.stats.successful_reads == 1u &&
          fixture.bridge.stats.bytes_read == sizeof expected &&
          fixture.bridge.stats.failures == 0u,
          "raw read statistics wrong");

    fixture_init();
    put_uio(TEST_IOV, 1, 91, 5u, 1u, 32, 1);
    for (i = 0u; i < sizeof expected; i++)
        fixture.ram.bytes[TEST_USER + i] = (uint8_t)(0xd0u + i);
    memcpy(expected, fixture.ram.bytes + TEST_USER, sizeof expected);
    CHECK(invoke() == ARM_SVC_HANDLED, "valid raw write was not handled");
    CHECK(memcmp(fixture.fake_block.bytes + 91u, expected,
                 sizeof expected) == 0, "raw write copied wrong bytes");
    CHECK(fixture.bridge.stats.successful_writes == 1u &&
          fixture.bridge.stats.bytes_written == sizeof expected,
          "raw write statistics wrong");
}

static void test_zero_and_multi_iovec_semantics(void) {
    uint8_t expected[25];

    fixture_init();
    put_uio(TEST_IOV, 4, 7, 5u, 0u, 12, 4);
    put_iov(0u, UINT32_C(0xdeadbeef), 0u);
    put_iov(1u, TEST_USER, 5u);
    put_iov(2u, UINT32_C(0xcafef00d), 0u);
    put_iov(3u, TEST_USER + 17u, 7u);
    memcpy(expected, fixture.fake_block.bytes + 7u, 12u);
    CHECK(invoke() == ARM_SVC_HANDLED, "multi-iovec read failed");
    CHECK(memcmp(fixture.ram.bytes + TEST_USER, expected, 5u) == 0 &&
          memcmp(fixture.ram.bytes + TEST_USER + 17u, expected + 5u, 7u) == 0,
          "multi-iovec scatter data wrong");
    CHECK(get_u32(TEST_IOV) == UINT32_C(0xdeadbeef) &&
          get_u32(TEST_IOV + 8u) == TEST_USER + 5u &&
          get_u32(TEST_IOV + 16u) == UINT32_C(0xcafef00d) &&
          get_u32(TEST_IOV + 24u) == TEST_USER + 24u &&
          get_u32(TEST_UIO + UIO_IOVS) == TEST_IOV + 24u,
          "zero-length advancement or final pointer differs from XNU");

    fixture_init();
    put_uio(TEST_IOV, 4, 31, 5u, 0u, 25, 4);
    put_iov(0u, UINT32_C(0xdeadbeef), 0u);
    put_iov(1u, TEST_USER, 20u);
    put_iov(2u, UINT32_C(0xcafef00d), 0u);
    put_iov(3u, TEST_USER + 64u, 20u);
    memcpy(expected, fixture.fake_block.bytes + 31u, sizeof expected);
    CHECK(invoke() == ARM_SVC_HANDLED &&
          memcmp(fixture.ram.bytes + TEST_USER, expected, 20u) == 0 &&
          memcmp(fixture.ram.bytes + TEST_USER + 64u,
                 expected + 20u, 5u) == 0,
          "residual-capped multi-iovec read copied the wrong prefix");
    CHECK(get_u32(TEST_IOV + 8u) == TEST_USER + 20u &&
          get_u32(TEST_IOV + 12u) == 0u &&
          get_u32(TEST_IOV + 24u) == TEST_USER + 69u &&
          get_u32(TEST_IOV + 28u) == 15u &&
          get_u32(TEST_UIO + UIO_IOVS) == TEST_IOV + 24u &&
          get_u32(TEST_UIO + UIO_IOVCNT) == 1u &&
          get_u32(TEST_UIO + UIO_OFFSET_LO) == 56u &&
          get_u32(TEST_UIO + UIO_RESID) == 0u,
          "partial iovec commit differs from XNU uio_update");

    fixture_init();
    put_uio(TEST_IOV, 1, 0, 5u, 0u, 4, 1);
    put_iov(0u, TEST_USER, UINT32_MAX);
    CHECK(invoke() == ARM_SVC_HANDLED &&
          get_u32(TEST_IOV) == TEST_USER + 4u &&
          get_u32(TEST_IOV + 4u) == UINT32_MAX - 4u &&
          get_u32(TEST_UIO + UIO_IOVCNT) == 1u,
          "large iovec with a bounded residual was rejected or over-consumed");

    fixture_init();
    put_uio(TEST_IOV, 1, 19, 5u, 0u, 0, 1);
    put_iov(0u, UINT32_C(0xffffffff), 0u);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 0u &&
          get_u32(TEST_UIO + UIO_IOVCNT) == 1u &&
          get_u32(TEST_UIO + UIO_IOVS) == TEST_IOV &&
          fixture.fake_block.read.calls == 0u &&
          fixture.bridge.stats.zero_length_requests == 1u,
          "zero residual did not remain an exact no-op");
}

static void test_all_user_segment_variants(void) {
    static const uint32_t segments[] = {0u, 1u, 3u, 5u, 6u,
                                        7u, 8u, 9u, 10u};
    size_t i;

    for (i = 0u; i < sizeof segments / sizeof segments[0]; i++) {
        fixture_init();
        put_uio(TEST_IOV, 1, 0, segments[i], 0u, 4, 1);
        put_iov(0u, TEST_USER, 4u);
        CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 0u &&
              get_u32(TEST_UIO + UIO_SEGFLG) == segments[i],
              "valid read uio segment %u was rejected or changed", segments[i]);

        fixture_init();
        put_uio(TEST_IOV, 1, 0, segments[i], 1u, 4, 1);
        put_iov(0u, TEST_USER, 4u);
        memset(fixture.ram.bytes + TEST_USER, (int)(0x40u + i), 4u);
        CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 0u &&
              get_u32(TEST_UIO + UIO_SEGFLG) == segments[i] &&
              memcmp(fixture.fake_block.bytes,
                     fixture.ram.bytes + TEST_USER, 4u) == 0,
              "valid write uio segment %u was rejected or changed",
              segments[i]);
    }
}

static void test_page_split_and_max_iovec_bound(void) {
    uint32_t i;

    fixture_init();
    put_uio(TEST_IOV, 1, 0, 5u, 0u, 32, 1);
    put_iov(0u, UINT32_C(0x00004ff0), 32u);
    CHECK(invoke() == ARM_SVC_HANDLED &&
          fixture.bridge.data_span_count == 2u &&
          memcmp(fixture.ram.bytes + UINT32_C(0x4ff0),
                 fixture.fake_block.bytes, 32u) == 0,
          "cross-page user buffer was not split safely");

    fixture_init();
    put_uio(UINT32_C(0x00006000), (int32_t)MD_RAW_BRIDGE_MAX_IOVECS,
            0, 5u, 0u, (int32_t)MD_RAW_BRIDGE_MAX_IOVECS,
            (int32_t)MD_RAW_BRIDGE_MAX_IOVECS);
    /* put_iov uses TEST_IOV, so populate this alternate array directly. */
    for (i = 0u; i < MD_RAW_BRIDGE_MAX_IOVECS; i++) {
        put_u32(UINT32_C(0x00006000) + i * 8u,
                UINT32_C(0x00009000) + i);
        put_u32(UINT32_C(0x00006000) + i * 8u + 4u, 1u);
    }
    CHECK(invoke() == ARM_SVC_HANDLED &&
          fixture.bridge.iov_plan_count == MD_RAW_BRIDGE_MAX_IOVECS &&
          fixture.bridge.data_span_count == MD_RAW_BRIDGE_MAX_IOVECS &&
          get_u32(TEST_UIO + UIO_IOVS) ==
              UINT32_C(0x00006000) +
              (MD_RAW_BRIDGE_MAX_IOVECS - 1u) * 8u,
          "exact 1024-iovec boundary failed");

    fixture_init();
    put_uio(UINT32_C(0x00006000), (int32_t)MD_RAW_BRIDGE_MAX_IOVECS,
            0, 5u, 0u, (int32_t)(MD_RAW_BRIDGE_MAX_IOVECS * 2u),
            (int32_t)MD_RAW_BRIDGE_MAX_IOVECS);
    for (i = 0u; i < MD_RAW_BRIDGE_MAX_IOVECS; i++) {
        put_u32(UINT32_C(0x00006000) + i * 8u,
                UINT32_C(0x00004fff));
        put_u32(UINT32_C(0x00006000) + i * 8u + 4u, 2u);
    }
    CHECK(invoke() == ARM_SVC_HANDLED &&
          fixture.bridge.data_span_count ==
              MD_RAW_BRIDGE_MAX_IOVECS * 2u,
          "1024 page-straddling iovecs exhausted a valid bounded plan");

    fixture_init();
    put_uio(UINT32_C(0x000063fc), (int32_t)MD_RAW_BRIDGE_MAX_IOVECS,
            0, 5u, 0u, (int32_t)MD_RAW_BRIDGE_MAX_IOVECS,
            (int32_t)MD_RAW_BRIDGE_MAX_IOVECS);
    for (i = 0u; i < MD_RAW_BRIDGE_MAX_IOVECS; i++) {
        put_u32(UINT32_C(0x000063fc) + i * 8u,
                UINT32_C(0x0000a000) + i);
        put_u32(UINT32_C(0x000063fc) + i * 8u + 4u, 1u);
    }
    CHECK(invoke() == ARM_SVC_HANDLED &&
          fixture.bridge.iov_plan_count == MD_RAW_BRIDGE_MAX_IOVECS,
          "unaligned 1024-iovec metadata exhausted its bounded mapping plan");

    fixture_init();
    put_uio(TEST_IOV, (int32_t)MD_RAW_BRIDGE_MAX_IOVECS + 1,
            0, 5u, 0u, 1, (int32_t)MD_RAW_BRIDGE_MAX_IOVECS + 1);
    expect_halt_error(MD_RAW_BRIDGE_ERROR_IOV_COUNT,
                      "1025 iovecs");
}

static void test_ttbr_split_kernel_metadata_and_user_buffer(void) {
    const uint32_t ttbr0 = UINT32_C(0x00018000);
    const uint32_t ttbr1 = UINT32_C(0x0001c000);
    const uint32_t kernel_uio = UINT32_C(0xc1001000);
    const uint32_t kernel_iov = UINT32_C(0xc1001100);
    const uint32_t user_buffer = UINT32_C(0x00104000);
    uint8_t expected[32];

    fixture_init();
    /* N=2 selects TTBR0 for 00xxxxxxxx and TTBR1 for c1xxxxxx. */
    put_u32(ttbr0 + UINT32_C(1) * 4u, UINT32_C(0x00000c02));
    put_u32(ttbr1 + UINT32_C(0xc10) * 4u, UINT32_C(0x00000c02));
    fixture.cpu.cp15.ttbr0 = ttbr0;
    fixture.cpu.cp15.ttbr1 = ttbr1;
    fixture.cpu.cp15.ttbcr = 2u;
    fixture.cpu.r[1] = kernel_uio;
    put_uio(kernel_iov, 1, 23, 5u, 0u, 32, 1);
    put_iov(0u, user_buffer, 32u);
    memcpy(expected, fixture.fake_block.bytes + 23u, sizeof expected);

    CHECK(invoke() == ARM_SVC_HANDLED &&
          memcmp(fixture.ram.bytes + TEST_USER, expected,
                 sizeof expected) == 0,
          "TTBR0 user/TTBR1 kernel split transfer failed");
    CHECK(get_u32(TEST_UIO + UIO_IOVCNT) == 0u &&
          get_u32(TEST_UIO + UIO_OFFSET_LO) == 55u &&
          get_u32(TEST_IOV + 4u) == 0u,
          "TTBR-split uio commit reached the wrong physical pages");

    fixture_init();
    put_u32(ttbr0 + UINT32_C(1) * 4u, UINT32_C(0x00000c02));
    put_u32(ttbr1 + UINT32_C(0xc10) * 4u, UINT32_C(0x00000c02));
    fixture.cpu.cp15.ttbr0 = ttbr0;
    fixture.cpu.cp15.ttbr1 = ttbr1;
    fixture.cpu.cp15.ttbcr = 2u;
    fixture.cpu.r[1] = kernel_uio;
    put_uio(kernel_iov, 1, 23, 5u, 0u, 32, 1);
    put_iov(0u, UINT32_C(0xc1004000), 32u);
    memset(fixture.ram.bytes + TEST_USER, 0xa5, 32u);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 14u &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_USER_ADDRESS &&
          fixture.fake_block.read.calls == 0u &&
          get_u32(TEST_UIO + UIO_RESID) == 32u,
          "manager-domain TTBR1 kernel buffer bypassed the user ceiling");
}

static void test_legacy_subpage_permissions(void) {
    const uint32_t l1 = UINT32_C(0x0001c000);
    const uint32_t l2 = UINT32_C(0x00017000);
    const uint32_t kernel_l1 = UINT32_C(0x00018000);
    const uint32_t kernel_uio = UINT32_C(0xc1001000);
    const uint32_t kernel_iov = UINT32_C(0xc1001100);
    const uint32_t user_va = UINT32_C(0x001043f0);
    const uint32_t user_pa = UINT32_C(0x000043f0);
    const uint32_t uio_va = UINT32_C(0x001043e0);
    const uint32_t uio_pa = UINT32_C(0x000013e0);
    uint8_t before[32];

    fixture_init();
    put_u32(l1 + UINT32_C(1) * 4u, (l2 & UINT32_C(0xfffffc00)) | 1u);
    /* Legacy small page: subpage 0 user-RW, subpage 1 user-RO. */
    put_u32(l2 + UINT32_C(4) * 4u,
            UINT32_C(0x00004000) | (3u << 4) | (2u << 6) |
            (3u << 8) | (3u << 10) | 2u);
    put_u32(kernel_l1 + UINT32_C(0xc10) * 4u, UINT32_C(0x00000c02));
    fixture.cpu.cp15.ttbr0 = l1;
    fixture.cpu.cp15.ttbr1 = kernel_l1;
    fixture.cpu.cp15.ttbcr = 2u;
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.cp15.sctlr &= ~ARM_SCTLR_XP;
    fixture.cpu.r[1] = kernel_uio;
    put_uio(kernel_iov, 1, 0, 5u, 0u, 32, 1);
    put_iov(0u, user_va, 32u);
    memset(fixture.ram.bytes + user_pa, 0x5a, sizeof before);
    memcpy(before, fixture.ram.bytes + user_pa, sizeof before);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 14u &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_USER_TRANSLATION &&
          fixture.fake_block.read.calls == 0u &&
          memcmp(before, fixture.ram.bytes + user_pa, sizeof before) == 0,
          "raw read crossed a denied legacy 1 KiB AP subpage");

    fixture_init();
    put_u32(l1 + UINT32_C(1) * 4u, (l2 & UINT32_C(0xfffffc00)) | 1u);
    /* Legacy small page: subpage 0 user-readable, subpage 1 privileged-only. */
    put_u32(l2 + UINT32_C(4) * 4u,
            UINT32_C(0x00004000) | (2u << 4) | (1u << 6) |
            (2u << 8) | (2u << 10) | 2u);
    put_u32(kernel_l1 + UINT32_C(0xc10) * 4u, UINT32_C(0x00000c02));
    fixture.cpu.cp15.ttbr0 = l1;
    fixture.cpu.cp15.ttbr1 = kernel_l1;
    fixture.cpu.cp15.ttbcr = 2u;
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.cp15.sctlr &= ~ARM_SCTLR_XP;
    fixture.cpu.r[1] = kernel_uio;
    put_uio(kernel_iov, 1, 0, 5u, 1u, 32, 1);
    put_iov(0u, user_va, 32u);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 14u &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_USER_TRANSLATION &&
          fixture.fake_block.write.calls == 0u,
          "raw write crossed a denied legacy 1 KiB AP subpage");

    fixture_init();
    put_u32(l1 + UINT32_C(1) * 4u, (l2 & UINT32_C(0xfffffc00)) | 1u);
    /* The 40-byte uio crosses from privileged-RW AP=01 into no-access AP=00. */
    put_u32(l2 + UINT32_C(4) * 4u,
            UINT32_C(0x00001000) | (1u << 4) | (0u << 6) |
            (1u << 8) | (1u << 10) | 2u);
    fixture.cpu.cp15.ttbr0 = l1;
    fixture.cpu.cp15.ttbcr = 0u;
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.cp15.sctlr &= ~ARM_SCTLR_XP;
    fixture.cpu.r[1] = uio_va;
    put_uio_at(uio_pa, TEST_IOV, 1, 0, 5u, 0u, 32, 1);
    expect_halt_error(MD_RAW_BRIDGE_ERROR_UIO_TRANSLATION,
                      "uio crossing denied legacy AP subpage");
}

static void test_guest_errors_are_atomic(void) {
    uint8_t before[32];

    fixture_init();
    fixture.bridge.config.user_address_limit = UINT32_C(0x00010000);
    put_uio(TEST_IOV, 1, 0, 5u, 0u, 16, 1);
    put_iov(0u, UINT32_C(0x0000fff0), 16u);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 0u,
          "half-open user range ending exactly at the ceiling was rejected");

    fixture_init();
    fixture.bridge.config.user_address_limit = UINT32_C(0x00010000);
    put_uio(TEST_IOV, 1, 0, 5u, 0u, 17, 1);
    put_iov(0u, UINT32_C(0x0000fff0), 17u);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 14u &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_USER_ADDRESS &&
          fixture.bridge.last_error.fault_va == UINT32_C(0x0000fff0) &&
          fixture.fake_block.read.calls == 0u,
          "user range crossing the exclusive ceiling was accepted");

    fixture_init();
    memcpy(before, fixture.ram.bytes + TEST_USER, sizeof before);
    fixture.cpu.r[0] ^= 1u;
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 6u &&
          fixture.fake_block.read.calls == 0u &&
          memcmp(before, fixture.ram.bytes + TEST_USER, sizeof before) == 0,
          "unsupported device did not return atomic ENXIO");

    fixture_init();
    put_uio(TEST_IOV, 1, -1, 5u, 0u, 32, 1);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 22u &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_OFFSET &&
          fixture.fake_block.read.calls == 0u,
          "negative offset did not return EINVAL");

    fixture_init();
    put_uio(TEST_IOV, 1, TEST_MEDIA_SIZE - 16, 5u, 0u, 32, 1);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 22u &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_MEDIA_RANGE,
          "out-of-media request did not return EINVAL");

    fixture_init();
    /* Client-domain AP=2 permits user reads but rejects user writes. */
    put_u32(TEST_L1, UINT32_C(0x00000802));
    fixture.cpu.cp15.dacr = 1u;
    memcpy(before, fixture.ram.bytes + TEST_USER, sizeof before);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 14u &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_USER_TRANSLATION &&
          (fixture.bridge.last_error.mmu_status & (1u << 11)) != 0u &&
          fixture.fake_block.read.calls == 0u &&
          memcmp(before, fixture.ram.bytes + TEST_USER, sizeof before) == 0 &&
          get_u32(TEST_UIO + UIO_RESID) == 32u,
          "read-to-read-only user page was not atomic EFAULT");

    fixture_init();
    put_uio(TEST_IOV, 1, 0, 5u, 1u, 32, 1);
    memset(fixture.ram.bytes + TEST_USER, 0x5a, 32u);
    put_u32(TEST_L1, UINT32_C(0x00000802));
    fixture.cpu.cp15.dacr = 1u;
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 0u &&
          memcmp(fixture.fake_block.bytes,
                 fixture.ram.bytes + TEST_USER, 32u) == 0,
          "write-from-user incorrectly required user write permission");
}

static void test_malformed_uio_fails_closed(void) {
    fixture_init();
    fixture.cpu.cp15.sctlr &= ~ARM_SCTLR_M;
    expect_halt_error(MD_RAW_BRIDGE_ERROR_MMU_DISABLED, "MMU disabled");

    fixture_init();
    fixture.cpu.r[1] = TEST_UIO + 2u;
    expect_halt_error(MD_RAW_BRIDGE_ERROR_UIO_ALIGNMENT, "unaligned uio");

    fixture_init();
    put_u32(TEST_UIO + UIO_FLAGS, 0u);
    expect_halt_error(MD_RAW_BRIDGE_ERROR_UIO_STATE, "uninited uio");

    fixture_init();
    put_u32(TEST_UIO + UIO_SEGFLG, 2u);
    expect_halt_error(MD_RAW_BRIDGE_ERROR_SEGMENT,
                      "entry received unsupported system-space segment");

    fixture_init();
    put_u32(TEST_UIO + UIO_RW, 2u);
    expect_halt_error(MD_RAW_BRIDGE_ERROR_DIRECTION, "invalid direction");

    fixture_init();
    put_u32(TEST_UIO + UIO_RESID, MD_RAW_BRIDGE_MAX_TRANSFER + 1u);
    put_u32(TEST_IOV + 4u, MD_RAW_BRIDGE_MAX_TRANSFER + 1u);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 22u &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_RESIDUAL &&
          fixture.fake_block.read.calls == 0u &&
          get_u32(TEST_UIO + UIO_RESID) == MD_RAW_BRIDGE_MAX_TRANSFER + 1u,
          "oversized bounded transfer did not return atomic EINVAL");

    fixture_init();
    put_u32(TEST_UIO + UIO_RESID, UINT32_MAX);
    expect_halt_error(MD_RAW_BRIDGE_ERROR_RESIDUAL, "negative residual");

    fixture_init();
    put_u32(TEST_UIO + UIO_IOVCNT, 0u);
    expect_halt_error(MD_RAW_BRIDGE_ERROR_IOV_COUNT, "zero iov count");

    fixture_init();
    put_u32(TEST_IOV + 4u, 31u);
    expect_halt_error(MD_RAW_BRIDGE_ERROR_IOV_SUM,
                      "short iov sum would hang guest uiomove");

    fixture_init();
    put_iov(0u, UINT32_C(0xfffffff0), 32u);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 14u &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_USER_ADDRESS,
          "wrapping user address did not return EFAULT");

    fixture_init();
    put_iov(0u, TEST_UIO, 32u);
    expect_halt_error(MD_RAW_BRIDGE_ERROR_METADATA_ALIAS,
                      "user/uio physical alias");

    fixture_init();
    put_u32(TEST_UIO + UIO_IOVS, TEST_UIO);
    expect_halt_error(MD_RAW_BRIDGE_ERROR_METADATA_ALIAS,
                      "iov/uio physical alias");
}

static void test_backend_failures_do_not_commit_uio(void) {
    uint8_t before[32];

    fixture_init();
    memset(fixture.ram.bytes + TEST_USER, 0xcc, sizeof before);
    memcpy(before, fixture.ram.bytes + TEST_USER, sizeof before);
    fixture.fake_block.read.mode = IO_PARTIAL_ERROR;
    fixture.fake_block.read.first_chunk = 5u;
    expect_halt_error(MD_RAW_BRIDGE_ERROR_BLOCK_IO,
                      "partial backend read");
    CHECK(fixture.bridge.last_error.transferred == 5u &&
          memcmp(before, fixture.ram.bytes + TEST_USER, sizeof before) == 0 &&
          get_u32(TEST_UIO + UIO_RESID) == 32u &&
          get_u32(TEST_IOV + 4u) == 32u,
          "failed read leaked scratch or committed uio");

    fixture_init();
    put_uio(TEST_IOV, 1, 0, 5u, 1u, 32, 1);
    memset(fixture.ram.bytes + TEST_USER, 0x6b, 32u);
    fixture.fake_block.write.mode = IO_PARTIAL_ERROR;
    fixture.fake_block.write.first_chunk = 6u;
    expect_halt_error(MD_RAW_BRIDGE_ERROR_BLOCK_IO,
                      "partial backend write");
    CHECK(fixture.bridge.last_error.transferred == 6u &&
          memcmp(fixture.fake_block.bytes,
                 fixture.ram.bytes + TEST_USER, 6u) == 0 &&
          get_u32(TEST_UIO + UIO_RESID) == 32u &&
          get_u32(TEST_IOV + 4u) == 32u,
          "partial write diagnostics or uio rollback wrong");

    fixture_init();
    fixture.fake_block.read.mode = IO_ALWAYS_ZERO;
    expect_halt_error(MD_RAW_BRIDGE_ERROR_BLOCK_IO,
                      "zero-progress backend read");
    CHECK(fixture.bridge.last_error.block_status == VM_BLOCK_STATUS_STALLED,
          "zero-progress status was not preserved");

    fixture_init();
    fixture.fake_block.read.mode = IO_OVERREPORT;
    expect_halt_error(MD_RAW_BRIDGE_ERROR_BLOCK_IO,
                      "over-reporting backend read");
    CHECK(fixture.bridge.last_error.block_status == VM_BLOCK_STATUS_PROTOCOL,
          "over-report protocol status was not preserved");
}

static void test_exact_gate_and_configuration(void) {
    md_raw_bridge_config_t config;
    md_raw_bridge_stats_t stats;

    fixture_init();
    stats = fixture.bridge.stats;
    fixture.cpu.cpsr = ARM_MODE_USR | ARM_CPSR_T;
    CHECK(invoke() == ARM_SVC_UNHANDLED, "user-mode raw SVC was consumed");
    fixture.cpu.cpsr = ARM_MODE_SVC;
    CHECK(invoke() == ARM_SVC_UNHANDLED, "A32 raw SVC was consumed");
    fixture.cpu.cpsr = ARM_MODE_SVC | ARM_CPSR_T;
    CHECK(md_raw_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                                   TEST_RAW_PC + 2u, TEST_RAW_SVC) ==
              ARM_SVC_UNHANDLED &&
          md_raw_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                                   TEST_RAW_PC, TEST_RAW_SVC ^ 1u) ==
              ARM_SVC_UNHANDLED &&
          memcmp(&stats, &fixture.bridge.stats, sizeof stats) == 0,
          "wrong raw site/encoding changed bridge state");

    fixture_init();
    config = fixture.bridge.config;
    CHECK(md_raw_bridge_config_valid(&config), "default config rejected");
    config.site.pc |= 1u;
    CHECK(!md_raw_bridge_config_valid(&config), "odd patch PC accepted");
    config = fixture.bridge.config;
    config.site.encoding = UINT32_C(0xbe00);
    CHECK(!md_raw_bridge_config_valid(&config), "non-SVC encoding accepted");
    config = fixture.bridge.config;
    config.media_size--;
    CHECK(!md_raw_bridge_config_valid(&config), "block size drift accepted");
    config = fixture.bridge.config;
    config.media_size = (uint64_t)INT64_MAX + UINT64_C(1);
    fixture.block.size = config.media_size;
    CHECK(!md_raw_bridge_config_valid(&config),
          "media larger than signed XNU off_t accepted");
    fixture.block.size = TEST_MEDIA_SIZE;
    config = fixture.bridge.config;
    config.ram_size--;
    CHECK(!md_raw_bridge_config_valid(&config), "unaligned RAM size accepted");
    config = fixture.bridge.config;
    config.user_address_limit = 0u;
    CHECK(!md_raw_bridge_config_valid(&config), "zero user ceiling accepted");
    config = fixture.bridge.config;
    config.user_address_limit--;
    CHECK(!md_raw_bridge_config_valid(&config),
          "unaligned user ceiling accepted");
    CHECK(!md_raw_bridge_config_valid(NULL), "null config accepted");

    fixture_init();
    fixture.block.read_at = NULL;
    expect_halt_error(MD_RAW_BRIDGE_ERROR_INVALID_CONFIG,
                      "missing backend read callback");
    fixture_init();
    fixture.cpu.bus = NULL;
    expect_halt_error(MD_RAW_BRIDGE_ERROR_MISSING_BUS_ACCESS,
                      "missing page-table bus");
}

static void test_exact_svc_bx_lr_pair_and_halt_rollback(void) {
    arm_cpu_t before;
    arm_cpu_t normalized;
    arm_status_t status;

    fixture_init();
    put_uio(TEST_IOV, 1, 0, 5u, 0u, 4, 1);
    put_iov(0u, TEST_USER, 4u);
    put_u32(TEST_RAW_PC, UINT32_C(0x4770dfe3));
    arm_bus_set_privileged_svc_handler(&fixture.bus,
                                       md_raw_bridge_handle_svc,
                                       &fixture.bridge);
    fixture.cpu.r[15] = TEST_RAW_PC;
    before = fixture.cpu;
    status = arm_step(&fixture.cpu);
    normalized = fixture.cpu;
    normalized.r[0] = before.r[0];
    normalized.r[15] = before.r[15];
    normalized.cycles = before.cycles;
    CHECK(status == ARM_OK && fixture.cpu.r[0] == 0u &&
          fixture.cpu.r[15] == TEST_RAW_PC + 2u &&
          fixture.cpu.cycles == before.cycles + 1u &&
          memcmp(&normalized, &before, sizeof before) == 0,
          "SVC did not preserve all state except r0/pc/cycles");
    status = arm_step(&fixture.cpu);
    CHECK(status == ARM_OK && fixture.cpu.r[15] == UINT32_C(0x00003500) &&
          (fixture.cpu.cpsr & ARM_CPSR_T) != 0u &&
          fixture.cpu.cycles == before.cycles + 2u,
          "BX LR did not return to the audited Thumb caller");

    fixture_init();
    put_uio(TEST_IOV, 1, 0, 5u, 0u, 4, 1);
    put_iov(0u, TEST_USER, 4u);
    put_u32(TEST_RAW_PC, UINT32_C(0x4770dfe3));
    fixture.fake_block.read.mode = IO_ERROR;
    arm_bus_set_privileged_svc_handler(&fixture.bus,
                                       md_raw_bridge_handle_svc,
                                       &fixture.bridge);
    fixture.cpu.r[15] = TEST_RAW_PC;
    fixture.cpu.cycles = UINT64_MAX;
    before = fixture.cpu;
    status = arm_step(&fixture.cpu);
    CHECK(status == ARM_HALT &&
          memcmp(&fixture.cpu, &before, sizeof before) == 0 &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_BLOCK_IO,
          "backend error did not halt with exact CPU rollback");
}

static void test_saturating_diagnostics_and_strings(void) {
    fixture_init();
    fixture.bridge.stats.successful_reads = UINT64_MAX;
    fixture.bridge.stats.bytes_read = UINT64_MAX - 1u;
    put_uio(TEST_IOV, 1, 0, 5u, 0u, 4, 1);
    put_iov(0u, TEST_USER, 4u);
    CHECK(invoke() == ARM_SVC_HANDLED &&
          fixture.bridge.stats.successful_reads == UINT64_MAX &&
          fixture.bridge.stats.bytes_read == UINT64_MAX,
          "raw statistics wrapped instead of saturating");

    fixture_init();
    fixture.bridge.stats.guest_errors = UINT64_MAX;
    fixture.cpu.r[0] ^= 1u;
    CHECK(invoke() == ARM_SVC_HANDLED &&
          fixture.bridge.stats.guest_errors == UINT64_MAX &&
          fixture.bridge.last_guest_error.code == MD_RAW_BRIDGE_ERROR_DEVICE &&
          fixture.bridge.last_guest_error.guest_errno == 6,
          "guest-error counter wrapped or dedicated diagnostic was lost");
    fixture.fake_block.read.mode = IO_ERROR;
    fixture.cpu.r[0] = TEST_DEVICE;
    CHECK(invoke() == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_BLOCK_IO &&
          fixture.bridge.last_guest_error.code == MD_RAW_BRIDGE_ERROR_DEVICE &&
          fixture.bridge.last_guest_error.guest_errno == 6,
          "fatal error overwrote the last guest-errno diagnostic");
    CHECK(strcmp(md_raw_bridge_error_string(MD_RAW_BRIDGE_ERROR_BLOCK_IO),
                 "block transfer failed") == 0 &&
          strcmp(md_raw_bridge_error_string(MD_RAW_BRIDGE_ERROR_USER_ADDRESS),
                 "user address outside configured user VA range") == 0 &&
          strcmp(md_raw_bridge_error_string((md_raw_bridge_error_code_t)999),
                 "unknown raw bridge error") == 0,
          "raw error strings wrong");
    CHECK(md_raw_bridge_handle_svc(NULL, &fixture.cpu,
                                   TEST_RAW_PC, TEST_RAW_SVC) == ARM_SVC_ERROR,
          "null bridge did not fail closed");
    CHECK(md_raw_bridge_handle_svc(&fixture.bridge, NULL,
                                   TEST_RAW_PC, TEST_RAW_SVC) == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_NULL_CPU,
          "null CPU diagnostic wrong");
    md_raw_bridge_init(NULL, NULL);
}

int main(void) {
    printf("iOS3-VM raw md bridge tests\n");
    test_read_write_and_exact_uio_commit();
    test_zero_and_multi_iovec_semantics();
    test_all_user_segment_variants();
    test_page_split_and_max_iovec_bound();
    test_ttbr_split_kernel_metadata_and_user_buffer();
    test_legacy_subpage_permissions();
    test_guest_errors_are_atomic();
    test_malformed_uio_fails_closed();
    test_backend_failures_do_not_commit_uio();
    test_exact_gate_and_configuration();
    test_exact_svc_bx_lr_pair_and_halt_rollback();
    test_saturating_diagnostics_and_strings();
    printf("\n%u passed, %u failed\n", g_passes, g_failures);
    return g_failures == 0u ? 0 : 1;
}

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

#define TEST_RAM_SIZE UINT32_C(1048576)
#define TEST_MEDIA_SIZE UINT32_C(131072)
#define TEST_L1 UINT32_C(0x0001c000)
#define TEST_RAW_PC UINT32_C(0x00003000)
#define TEST_RAW_SVC UINT32_C(0x0000dfe3)
#define TEST_RAW_COMPLETION_PC (TEST_RAW_PC + UINT32_C(2))
#define TEST_RAW_COMPLETION_SVC UINT32_C(0x0000dfe4)
#define TEST_UIOMOVE_PC UINT32_C(0x00003800)
#define TEST_BOUNCE_PA UINT32_C(0x00040000)
#define TEST_BOUNCE_STRIDE MD_RAW_BRIDGE_MAX_TRANSFER
#define TEST_BOUNCE_SLOTS UINT32_C(4)
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
    uint32_t base;
    uint8_t bytes[TEST_RAM_SIZE];
    uint64_t read32_calls;
} fake_ram_t;

static bool ram_resolve(const fake_ram_t *ram, uint32_t address,
                        size_t length, size_t *offset) {
    uint64_t relative;
    if (address < ram->base)
        return false;
    relative = (uint64_t)address - ram->base;
    if (relative > TEST_RAM_SIZE ||
        length > (uint64_t)TEST_RAM_SIZE - relative)
        return false;
    *offset = (size_t)relative;
    return true;
}

static uint8_t ram_read8(void *context, uint32_t address) {
    fake_ram_t *ram = (fake_ram_t *)context;
    size_t offset;
    return ram_resolve(ram, address, 1u, &offset) ? ram->bytes[offset] : 0u;
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
    if (!ram_resolve(ram, address, 4u, &offset))
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
    if (ram_resolve(ram, address, 1u, &offset))
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

/* Static because the bridge owns bounded staging and coherent-tail buffers. */
static fixture_t fixture;

static void put_u32(uint32_t address, uint32_t value) {
    ram_write32(&fixture.ram, address, value);
}

static uint32_t get_u32(uint32_t address) {
    return ram_read32(&fixture.ram, address);
}

static bool bytes_are(const uint8_t *bytes, size_t length, uint8_t value) {
    size_t i;
    for (i = 0u; i < length; i++) {
        if (bytes[i] != value)
            return false;
    }
    return true;
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
    config.completion_site.pc = TEST_RAW_COMPLETION_PC;
    config.completion_site.encoding = TEST_RAW_COMPLETION_SVC;
    config.uiomove_thumb_pc = TEST_UIOMOVE_PC;
    config.expected_device = TEST_DEVICE;
    config.user_address_limit = TEST_USER_LIMIT;
    config.media_size = TEST_MEDIA_SIZE;
    config.ram_base = 0u;
    config.ram_size = TEST_RAM_SIZE;
    config.ram = fixture.ram.bytes;
    config.bounce_base_pa = TEST_BOUNCE_PA;
    config.bounce_stride = TEST_BOUNCE_STRIDE;
    config.bounce_slot_count = TEST_BOUNCE_SLOTS;
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

static arm_svc_result_t invoke_entry_raw(void) {
    return md_raw_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                                    TEST_RAW_PC, TEST_RAW_SVC);
}

static arm_svc_result_t invoke_completion(void) {
    return md_raw_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                                    TEST_RAW_COMPLETION_PC,
                                    TEST_RAW_COMPLETION_SVC);
}

static arm_svc_result_t invoke(void) {
    uint32_t return_pc = fixture.cpu.r[14] & ~UINT32_C(1);
    arm_svc_result_t result = invoke_entry_raw();
    /*
     * Most legacy assertions care about the Darwin result, not the new
     * control-flow contract. Normalize only an ordinary return; a native
     * uiomove redirect remains visible to continuation-specific tests.
     */
    if (result == ARM_SVC_REDIRECTED &&
        fixture.cpu.r[15] == return_pc)
        return ARM_SVC_HANDLED;
    return result;
}

static void simulate_uiomove_at(uint32_t uio_pa, uint32_t iov_pa,
                               uint32_t done, uint32_t guest_errno) {
    uint32_t base = get_u32(iov_pa);
    uint32_t length = get_u32(iov_pa + 4u);
    uint32_t residual = get_u32(uio_pa + UIO_RESID);
    uint64_t offset = get_u32(uio_pa + UIO_OFFSET_LO);

    offset |= (uint64_t)get_u32(uio_pa + UIO_OFFSET_HI) << 32;
    CHECK(done <= length && done <= residual,
          "test attempted impossible uiomove progress");
    put_u32(iov_pa, base + done);
    put_u32(iov_pa + 4u, length - done);
    put_u32(uio_pa + UIO_OFFSET_LO, (uint32_t)(offset + done));
    put_u32(uio_pa + UIO_OFFSET_HI,
            (uint32_t)((offset + done) >> 32));
    put_u32(uio_pa + UIO_RESID, residual - done);
    if (done == length)
        put_u32(uio_pa + UIO_IOVCNT, 0u);
    fixture.cpu.r[0] = guest_errno;
}

static void simulate_simple_uiomove(uint32_t done, uint32_t guest_errno) {
    simulate_uiomove_at(TEST_UIO, TEST_IOV, done, guest_errno);
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
                 sizeof(uint32_t) * 14u) == 0 &&
          fixture.cpu.r[15] == (before.r[14] & ~UINT32_C(1)) &&
          fixture.cpu.cpsr == before.cpsr,
          "raw read changed state beyond r0 and the required return redirect");
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

static void test_media_eof_semantics(void) {
    static const uint32_t run04_residual = UINT32_C(32768);
    static const uint32_t run04_available = UINT32_C(12288);
    uint8_t expected_write[23];
    uint8_t expected_tail[8];
    size_t i;

    /*
     * This is the request shape observed in run04: a 32 KiB read begins
     * 12 KiB before the media end. Native mdevrw has no logical EOF check:
     * the host bridge returns the media prefix plus a bounded zero guard tail
     * and consumes the complete request.
     */
    fixture_init();
    put_uio(TEST_IOV, 1, TEST_MEDIA_SIZE - run04_available,
            5u, 0u, (int32_t)run04_residual, 1);
    put_iov(0u, TEST_USER, run04_residual);
    memset(fixture.ram.bytes + TEST_USER, 0xa5, run04_residual);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 0u &&
          fixture.fake_block.read.calls == 1u,
          "run04-shaped crossing-EOF read was not a successful short read");
    CHECK(memcmp(fixture.ram.bytes + TEST_USER,
                 fixture.fake_block.bytes +
                     TEST_MEDIA_SIZE - run04_available,
                 run04_available) == 0 &&
          bytes_are(fixture.ram.bytes + TEST_USER + run04_available,
                    run04_residual - run04_available, 0u),
          "crossing-EOF read did not expose the initial-zero guard tail");
    CHECK(get_u32(TEST_IOV) == TEST_USER + run04_residual &&
          get_u32(TEST_IOV + 4u) == 0u &&
          get_u32(TEST_UIO + UIO_IOVS) == TEST_IOV &&
          get_u32(TEST_UIO + UIO_IOVCNT) == 0u &&
          get_u32(TEST_UIO + UIO_OFFSET_LO) ==
              TEST_MEDIA_SIZE + run04_residual - run04_available &&
          get_u32(TEST_UIO + UIO_OFFSET_HI) == 0u &&
          get_u32(TEST_UIO + UIO_RESID) == 0u,
          "run04-shaped read did not consume the complete guarded request");
    CHECK(fixture.bridge.stats.successful_reads == 1u &&
          fixture.bridge.stats.bytes_read == run04_residual &&
          fixture.bridge.stats.media_bytes_read == run04_available &&
          fixture.bridge.stats.guard_bytes_read ==
              run04_residual - run04_available &&
          fixture.bridge.stats.guest_errors == 0u,
          "crossing-EOF read statistics did not split media and guard bytes");

    /*
     * Exercise the symmetric write path across zero-length and multiple
     * iovecs. Twenty-three bytes reach media and the final two are consumed
     * into the coherent guard overlay.
     */
    fixture_init();
    put_uio(TEST_IOV, 4, TEST_MEDIA_SIZE - 23u, 5u, 1u, 25, 4);
    put_iov(0u, UINT32_C(0xdeadbeef), 0u);
    put_iov(1u, TEST_USER, 20u);
    put_iov(2u, UINT32_C(0xcafef00d), 0u);
    put_iov(3u, TEST_USER + 64u, 20u);
    for (i = 0u; i < 20u; i++)
        fixture.ram.bytes[TEST_USER + i] = (uint8_t)(0x30u + i);
    for (i = 0u; i < 20u; i++)
        fixture.ram.bytes[TEST_USER + 64u + i] = (uint8_t)(0x80u + i);
    memcpy(expected_write, fixture.ram.bytes + TEST_USER, 20u);
    memcpy(expected_write + 20u, fixture.ram.bytes + TEST_USER + 64u, 3u);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 0u &&
          fixture.fake_block.write.calls == 1u &&
          memcmp(fixture.fake_block.bytes + TEST_MEDIA_SIZE - 23u,
                 expected_write, sizeof expected_write) == 0,
          "multi-iovec crossing-EOF write copied the wrong prefix");
    CHECK(get_u32(TEST_IOV) == UINT32_C(0xdeadbeef) &&
          get_u32(TEST_IOV + 8u) == TEST_USER + 20u &&
          get_u32(TEST_IOV + 12u) == 0u &&
          get_u32(TEST_IOV + 16u) == UINT32_C(0xcafef00d) &&
          get_u32(TEST_IOV + 24u) == TEST_USER + 69u &&
          get_u32(TEST_IOV + 28u) == 15u &&
          get_u32(TEST_UIO + UIO_IOVS) == TEST_IOV + 24u &&
          get_u32(TEST_UIO + UIO_IOVCNT) == 1u &&
          get_u32(TEST_UIO + UIO_OFFSET_LO) == TEST_MEDIA_SIZE + 2u &&
          get_u32(TEST_UIO + UIO_RESID) == 0u,
          "multi-iovec guarded write committed the wrong uio state");
    CHECK(fixture.bridge.stats.successful_writes == 1u &&
          fixture.bridge.stats.bytes_written == 25u &&
          fixture.bridge.stats.media_bytes_written == 23u &&
          fixture.bridge.stats.guard_bytes_written == 2u &&
          fixture.bridge.stats.guest_errors == 0u,
          "crossing-EOF write statistics did not split media and guard bytes");

    /*
     * Exact-EOF requests inside the bounded guard consume their complete uio
     * without touching the block backend.
     */
    fixture_init();
    put_uio(TEST_IOV, 2, TEST_MEDIA_SIZE, 5u, 0u, 32, 2);
    put_iov(0u, TEST_USER, 32u);
    put_iov(1u, UINT32_C(0xffffffff), 0u);
    memset(fixture.ram.bytes + TEST_USER, 0xa5, 32u);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 0u &&
          fixture.fake_block.read.calls == 0u &&
          bytes_are(fixture.ram.bytes + TEST_USER, 32u, 0u) &&
          get_u32(TEST_UIO + UIO_OFFSET_LO) == TEST_MEDIA_SIZE + 32u &&
          get_u32(TEST_UIO + UIO_RESID) == 0u,
          "exact-EOF read did not consume an initial-zero guard request");
    CHECK(fixture.bridge.stats.successful_reads == 1u &&
          fixture.bridge.stats.bytes_read == 32u &&
          fixture.bridge.stats.media_bytes_read == 0u &&
          fixture.bridge.stats.guard_bytes_read == 32u,
          "exact-EOF read guard statistics wrong");

    /* Exact-EOF writes stay coherent in the bounded in-memory tail. */
    fixture_init();
    put_uio(TEST_IOV, 2, TEST_MEDIA_SIZE, 5u, 1u, 8, 2);
    put_iov(0u, TEST_USER, 3u);
    put_iov(1u, TEST_USER + 16u, 5u);
    for (i = 0u; i < 3u; i++)
        fixture.ram.bytes[TEST_USER + i] = (uint8_t)(0xc0u + i);
    for (i = 0u; i < 5u; i++)
        fixture.ram.bytes[TEST_USER + 16u + i] = (uint8_t)(0xd0u + i);
    memcpy(expected_tail, fixture.ram.bytes + TEST_USER, 3u);
    memcpy(expected_tail + 3u, fixture.ram.bytes + TEST_USER + 16u, 5u);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 0u &&
          fixture.fake_block.write.calls == 0u &&
          get_u32(TEST_UIO + UIO_OFFSET_LO) == TEST_MEDIA_SIZE + 8u &&
          get_u32(TEST_UIO + UIO_RESID) == 0u &&
          memcmp(fixture.bridge.guard_tail, expected_tail,
                 sizeof expected_tail) == 0,
          "exact-EOF write did not persist its coherent guard bytes");
    CHECK(fixture.bridge.stats.successful_writes == 1u &&
          fixture.bridge.stats.bytes_written == 8u &&
          fixture.bridge.stats.media_bytes_written == 0u &&
          fixture.bridge.stats.guard_bytes_written == 8u,
          "exact-EOF write guard statistics wrong");

    fixture.cpu.r[0] = TEST_DEVICE;
    fixture.cpu.r[1] = TEST_UIO;
    fixture.cpu.r[14] = UINT32_C(0x00003501);
    put_uio(TEST_IOV, 1, TEST_MEDIA_SIZE, 5u, 0u, 8, 1);
    put_iov(0u, TEST_USER + 128u, 8u);
    memset(fixture.ram.bytes + TEST_USER + 128u, 0xa5, 8u);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 0u &&
          fixture.fake_block.read.calls == 0u &&
          memcmp(fixture.ram.bytes + TEST_USER + 128u, expected_tail,
                 sizeof expected_tail) == 0 &&
          fixture.bridge.stats.guard_bytes_read == 8u,
          "exact-EOF guard write was not readable on the next request");
}

static void test_uiomove_redirect_and_completion(void) {
    const uint32_t kernel_uio = UINT32_C(0xc1001000);
    const uint32_t kernel_iov = UINT32_C(0xc1001100);
    const uint32_t user_buffer = UINT32_C(0x00104000);
    const uint32_t ttbr0 = UINT32_C(0x00018000);
    const uint32_t ttbr1 = UINT32_C(0x0001c000);
    uint8_t expected[32];
    size_t i;

    /*
     * A COW-style permission fault must stage a guarded READ and execute the
     * guest's native uiomove. Its successful E4 continuation restores the
     * original segment and caller state.
     */
    fixture_init();
    put_uio(TEST_IOV, 1, TEST_MEDIA_SIZE - 16u, 5u, 0u, 32, 1);
    put_iov(0u, TEST_USER, 32u);
    put_u32(TEST_L1, UINT32_C(0x00000802)); /* user RO, privileged RW */
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.r[13] = UINT32_C(0x00008000);
    fixture.cpu.r[14] = UINT32_C(0x00003601);
    memset(fixture.ram.bytes + TEST_USER, 0xa5, 32u);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[15] == TEST_UIOMOVE_PC &&
          fixture.cpu.r[0] == TEST_BOUNCE_PA &&
          fixture.cpu.r[1] == 0u &&
          fixture.cpu.r[2] == 32u &&
          fixture.cpu.r[3] == TEST_UIO &&
          fixture.cpu.r[14] == (TEST_RAW_COMPLETION_PC | 1u) &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 7u &&
          fixture.bridge.pending[0].active &&
          fixture.bridge.pending[0].key_sp == UINT32_C(0x00008000),
          "COW read did not establish the exact native-uiomove ABI");
    CHECK(memcmp(fixture.ram.bytes + TEST_BOUNCE_PA,
                 fixture.fake_block.bytes + TEST_MEDIA_SIZE - 16u,
                 16u) == 0 &&
          bytes_are(fixture.ram.bytes + TEST_BOUNCE_PA + 16u, 16u, 0u) &&
          fixture.bridge.stats.redirected_requests == 1u &&
          fixture.bridge.stats.successful_reads == 0u,
          "COW read bounce staging or pre-completion statistics wrong");

    memcpy(fixture.ram.bytes + TEST_USER,
           fixture.ram.bytes + TEST_BOUNCE_PA, 32u);
    simulate_simple_uiomove(32u, 0u);
    /* Exact firmware leaves LR at its final nested Thumb BL return. */
    fixture.cpu.r[14] = UINT32_C(0xc0128d97);
    CHECK(invoke_completion() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[15] == UINT32_C(0x00003600) &&
          fixture.cpu.r[14] == UINT32_C(0x00003601) &&
          fixture.cpu.r[0] == 0u &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 5u &&
          !fixture.bridge.pending[0].active,
          "successful read completion rejected clobbered LR or failed to return");
    CHECK(fixture.bridge.stats.redirected_completions == 1u &&
          fixture.bridge.stats.successful_reads == 1u &&
          fixture.bridge.stats.bytes_read == 32u &&
          fixture.bridge.stats.media_bytes_read == 16u &&
          fixture.bridge.stats.guard_bytes_read == 16u,
          "successful redirected read statistics wrong");

    /*
     * Omit the TTBR0 user section while retaining TTBR1 kernel metadata. This
     * models the run04 demand-page translation fault. Native uiomove fills the
     * bounce slot for WRITE; E4 then persists the completed media prefix.
     */
    fixture_init();
    put_u32(ttbr1 + UINT32_C(0xc10) * 4u, UINT32_C(0x00000c02));
    fixture.cpu.cp15.ttbr0 = ttbr0;
    fixture.cpu.cp15.ttbr1 = ttbr1;
    fixture.cpu.cp15.ttbcr = 2u;
    fixture.cpu.r[1] = kernel_uio;
    fixture.cpu.r[13] = UINT32_C(0x00008100);
    fixture.cpu.r[14] = UINT32_C(0x00003701);
    put_uio(kernel_iov, 1, 77, 5u, 1u, 32, 1);
    put_iov(0u, user_buffer, 32u);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[15] == TEST_UIOMOVE_PC &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 7u &&
          fixture.fake_block.write.calls == 0u,
          "demand-page write did not redirect before backend I/O");
    for (i = 0u; i < sizeof expected; i++) {
        expected[i] = (uint8_t)(0xb0u + i);
        fixture.ram.bytes[TEST_BOUNCE_PA + i] = expected[i];
    }
    simulate_simple_uiomove(32u, 0u);
    CHECK(invoke_completion() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[15] == UINT32_C(0x00003700) &&
          fixture.cpu.r[14] == UINT32_C(0x00003701) &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 5u &&
          fixture.fake_block.write.calls == 1u &&
          memcmp(fixture.fake_block.bytes + 77u, expected,
                 sizeof expected) == 0,
          "demand-page write completion did not persist and return");
    CHECK(fixture.bridge.stats.successful_writes == 1u &&
          fixture.bridge.stats.bytes_written == 32u &&
          fixture.bridge.stats.media_bytes_written == 32u &&
          fixture.bridge.stats.redirected_completions == 1u,
          "successful redirected write statistics wrong");

    /*
     * The host-resident tail is also coherent across native-uiomove
     * continuations; it never changes the block image's exact size.
     */
    fixture_init();
    put_uio(TEST_IOV, 1, TEST_MEDIA_SIZE + 17u, 5u, 1u, 32, 1);
    put_iov(0u, TEST_USER, 32u);
    put_u32(TEST_L1, UINT32_C(0x00000402)); /* user no-access */
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.r[13] = UINT32_C(0x00008200);
    fixture.cpu.r[14] = UINT32_C(0x00003801);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
          fixture.fake_block.write.calls == 0u,
          "tail WRITE did not redirect without touching the backend");
    for (i = 0u; i < sizeof expected; i++) {
        expected[i] = (uint8_t)(0xe0u + i);
        fixture.ram.bytes[TEST_BOUNCE_PA + i] = expected[i];
    }
    simulate_simple_uiomove(32u, 0u);
    fixture.cpu.r[14] = UINT32_C(0xc0128d97);
    CHECK(invoke_completion() == ARM_SVC_REDIRECTED &&
          memcmp(fixture.bridge.guard_tail + 17u, expected,
                 sizeof expected) == 0 &&
          fixture.bridge.stats.guard_bytes_written == 32u,
          "redirected tail WRITE was not committed to the guard overlay");

    put_uio(TEST_IOV, 1, TEST_MEDIA_SIZE + 17u, 5u, 0u, 32, 1);
    put_iov(0u, TEST_USER, 32u);
    put_u32(TEST_L1, UINT32_C(0x00000802)); /* user RO: READ needs COW */
    fixture.cpu.r[0] = TEST_DEVICE;
    fixture.cpu.r[1] = TEST_UIO;
    fixture.cpu.r[13] = UINT32_C(0x00008300);
    fixture.cpu.r[14] = UINT32_C(0x00003901);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
          memcmp(fixture.ram.bytes + TEST_BOUNCE_PA, expected,
                 sizeof expected) == 0 &&
          fixture.fake_block.read.calls == 0u,
          "redirected tail READ did not stage the coherent guard bytes");
    memcpy(fixture.ram.bytes + TEST_USER,
           fixture.ram.bytes + TEST_BOUNCE_PA, sizeof expected);
    simulate_simple_uiomove(32u, 0u);
    CHECK(invoke_completion() == ARM_SVC_REDIRECTED &&
          memcmp(fixture.ram.bytes + TEST_USER, expected,
                 sizeof expected) == 0 &&
          fixture.bridge.stats.guard_bytes_read == 32u &&
          fixture.bridge.stats.redirected_completions == 2u,
          "redirected guard write was not readable on the next continuation");
}

static void test_uiomove_segment_conversion(void) {
    static const uint32_t segments[] = {0u, 1u, 3u, 5u, 6u,
                                        7u, 8u, 9u, 10u};
    static const uint32_t physical[] = {3u, 3u, 3u, 7u, 3u,
                                        3u, 10u, 3u, 3u};
    size_t i;

    for (i = 0u; i < sizeof segments / sizeof segments[0]; i++) {
        fixture_init();
        put_uio(TEST_IOV, 1, 0, segments[i], 0u, 4, 1);
        put_iov(0u, TEST_USER, 4u);
        put_u32(TEST_L1, UINT32_C(0x00000802));
        fixture.cpu.cp15.dacr = 1u;
        fixture.cpu.r[13] = UINT32_C(0x00009000) + (uint32_t)i * 0x10u;
        CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
              fixture.cpu.r[15] == TEST_UIOMOVE_PC &&
              get_u32(TEST_UIO + UIO_SEGFLG) == physical[i],
              "segment %u converted to %u, expected %u",
              segments[i], get_u32(TEST_UIO + UIO_SEGFLG), physical[i]);
    }
}

static void test_nonzero_ram_base_redirect(void) {
    const uint32_t ram_base = UINT32_C(0x08000000);
    md_raw_bridge_config_t config;

    fixture_init();
    fixture.ram.base = ram_base;
    config = default_config();
    config.ram_base = ram_base;
    config.bounce_base_pa = ram_base + TEST_BOUNCE_PA;
    md_raw_bridge_init(&fixture.bridge, &config);

    put_uio_at(ram_base + TEST_UIO, TEST_IOV,
               1, 13, 5u, 0u, 32, 1);
    put_u32(ram_base + TEST_IOV, TEST_USER);
    put_u32(ram_base + TEST_IOV + 4u, 32u);
    put_u32(ram_base + TEST_L1, ram_base | UINT32_C(0x00000802));
    fixture.cpu.cpsr = ARM_MODE_SVC | ARM_CPSR_T;
    fixture.cpu.r[0] = TEST_DEVICE;
    fixture.cpu.r[1] = TEST_UIO;
    fixture.cpu.r[13] = ram_base + UINT32_C(0x00008000);
    fixture.cpu.r[14] = UINT32_C(0x00003a01);
    fixture.cpu.cp15.ttbr0 = ram_base + TEST_L1;
    fixture.cpu.cp15.ttbcr = 0u;
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.cp15.sctlr |= ARM_SCTLR_M;

    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[0] == ram_base + TEST_BOUNCE_PA &&
          memcmp(fixture.ram.bytes + TEST_BOUNCE_PA,
                 fixture.fake_block.bytes + 13u, 32u) == 0,
          "nonzero-RAM-base redirect staged the wrong physical bounce");
    memcpy(fixture.ram.bytes + TEST_USER,
           fixture.ram.bytes + TEST_BOUNCE_PA, 32u);
    simulate_uiomove_at(ram_base + TEST_UIO,
                        ram_base + TEST_IOV, 32u, 0u);
    fixture.cpu.r[14] = UINT32_C(0xc0128d97);
    CHECK(invoke_completion() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[15] == UINT32_C(0x00003a00) &&
          get_u32(ram_base + TEST_UIO + UIO_SEGFLG) == 5u &&
          memcmp(fixture.ram.bytes + TEST_USER,
                 fixture.fake_block.bytes + 13u, 32u) == 0 &&
          !fixture.bridge.pending[0].active,
          "nonzero-RAM-base completion did not restore and return");
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
    CHECK(invoke() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[15] == TEST_UIOMOVE_PC &&
          fixture.cpu.r[0] == TEST_BOUNCE_PA &&
          fixture.cpu.r[2] == 32u &&
          fixture.cpu.r[3] == kernel_uio &&
          fixture.cpu.r[14] == (TEST_RAW_COMPLETION_PC | 1u) &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 7u &&
          fixture.fake_block.read.calls == 1u &&
          memcmp(fixture.ram.bytes + TEST_BOUNCE_PA,
                 fixture.fake_block.bytes, 32u) == 0 &&
          memcmp(before, fixture.ram.bytes + user_pa, sizeof before) == 0,
          "raw read did not redirect a denied legacy COW subpage");

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
    CHECK(invoke() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[15] == TEST_UIOMOVE_PC &&
          fixture.cpu.r[0] == TEST_BOUNCE_PA &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 7u &&
          fixture.fake_block.write.calls == 0u,
          "raw write did not redirect a denied legacy user subpage");

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
    put_uio(TEST_IOV, 1,
            TEST_MEDIA_SIZE + MD_RAW_BRIDGE_MAX_TRANSFER - 16u,
            5u, 0u, 32, 1);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 22u &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_MEDIA_RANGE &&
          fixture.fake_block.read.calls == 0u &&
          get_u32(TEST_UIO + UIO_OFFSET_LO) ==
              TEST_MEDIA_SIZE + MD_RAW_BRIDGE_MAX_TRANSFER - 16u &&
          get_u32(TEST_UIO + UIO_RESID) == 32u,
          "read crossing the bounded guard did not return atomic EINVAL");

    fixture_init();
    put_uio(TEST_IOV, 1,
            TEST_MEDIA_SIZE + MD_RAW_BRIDGE_MAX_TRANSFER - 16u,
            5u, 1u, 32, 1);
    CHECK(invoke() == ARM_SVC_HANDLED && fixture.cpu.r[0] == 22u &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_MEDIA_RANGE &&
          fixture.fake_block.write.calls == 0u &&
          get_u32(TEST_UIO + UIO_OFFSET_LO) ==
              TEST_MEDIA_SIZE + MD_RAW_BRIDGE_MAX_TRANSFER - 16u &&
          get_u32(TEST_UIO + UIO_RESID) == 32u,
          "write crossing the bounded guard did not return atomic EINVAL");

    fixture_init();
    /* Client-domain AP=2 permits user reads but rejects user writes. */
    put_u32(TEST_L1, UINT32_C(0x00000802));
    fixture.cpu.cp15.dacr = 1u;
    memcpy(before, fixture.ram.bytes + TEST_USER, sizeof before);
    CHECK(invoke() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[15] == TEST_UIOMOVE_PC &&
          fixture.cpu.r[0] == TEST_BOUNCE_PA &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 7u &&
          fixture.fake_block.read.calls == 1u &&
          memcmp(before, fixture.ram.bytes + TEST_USER, sizeof before) == 0 &&
          get_u32(TEST_UIO + UIO_RESID) == 32u,
          "read-to-read-only COW page did not redirect through uiomove");

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

    fixture_init();
    fixture.cpu.r[1] = TEST_BOUNCE_PA;
    put_uio_at(TEST_BOUNCE_PA, TEST_IOV, 1, 0, 5u, 0u, 32, 1);
    expect_halt_error(MD_RAW_BRIDGE_ERROR_BOUNCE_ALIAS,
                      "uio aliases reserved bounce RAM");

    fixture_init();
    put_iov(0u, TEST_BOUNCE_PA, 32u);
    expect_halt_error(MD_RAW_BRIDGE_ERROR_BOUNCE_ALIAS,
                      "user data aliases reserved bounce RAM");
}

static void test_faultable_plan_scans_later_spans(void) {
    const uint32_t l2 = UINT32_C(0x00017000);
    const uint32_t small_page_ap3 = UINT32_C(0x00000ff2);

    /*
     * The first iovec is absent and needs native fault handling. A later,
     * already-resident iovec must still be validated before any redirect.
     */
    fixture_init();
    put_uio(TEST_IOV, 2, 0, 5u, 0u, 8, 2);
    put_iov(0u, UINT32_C(0x00005000), 4u);
    put_iov(1u, UINT32_C(0x00006000), 4u);
    put_u32(TEST_L1, (l2 & UINT32_C(0xfffffc00)) | 1u);
    put_u32(l2 + 1u * 4u, UINT32_C(0x00001000) | small_page_ap3);
    put_u32(l2 + 5u * 4u, 0u);
    put_u32(l2 + 6u * 4u, TEST_BOUNCE_PA | small_page_ap3);
    fixture.cpu.cp15.dacr = 1u;
    CHECK(invoke_entry_raw() == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_BOUNCE_ALIAS &&
          !fixture.bridge.pending[0].active &&
          fixture.fake_block.read.calls == 0u,
          "later bounce alias escaped validation after an earlier fault");

    fixture_init();
    put_uio(TEST_IOV, 2, 0, 5u, 0u, 8, 2);
    put_iov(0u, UINT32_C(0x00005000), 4u);
    put_iov(1u, UINT32_C(0x00006000), 4u);
    put_u32(TEST_L1, (l2 & UINT32_C(0xfffffc00)) | 1u);
    put_u32(l2 + 1u * 4u, UINT32_C(0x00001000) | small_page_ap3);
    put_u32(l2 + 5u * 4u, 0u);
    put_u32(l2 + 6u * 4u, UINT32_C(0x00001000) | small_page_ap3);
    fixture.cpu.cp15.dacr = 1u;
    CHECK(invoke_entry_raw() == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_METADATA_ALIAS &&
          !fixture.bridge.pending[0].active &&
          fixture.fake_block.read.calls == 0u,
          "later metadata alias escaped validation after an earlier fault");
}

static void test_pending_slots_collision_and_exhaustion(void) {
    uint32_t i;

    /* A same-kernel-stack E3 reentry is a collision even if it could go fast. */
    fixture_init();
    put_u32(TEST_L1, UINT32_C(0x00000802));
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.r[13] = UINT32_C(0x0000a000);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
          fixture.bridge.pending[0].active,
          "collision setup did not leave a pending continuation");
    put_u32(TEST_L1, UINT32_C(0x00000c02));
    fixture.cpu.cp15.dacr = 3u;
    put_uio_at(UINT32_C(0x00001200), TEST_UIO,
               1, 0, 5u, 0u, 4, 1);
    fixture.cpu.r[0] = TEST_DEVICE;
    fixture.cpu.r[1] = UINT32_C(0x00001200);
    fixture.cpu.r[14] = UINT32_C(0x00003501);
    fixture.cpu.r[13] = UINT32_C(0x0000a100);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[0] == 16u &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_PENDING_COLLISION &&
          fixture.bridge.pending[0].active,
          "overlapping pending iov metadata did not return bounded EBUSY");
    fixture.cpu.r[0] = TEST_DEVICE;
    fixture.cpu.r[13] = UINT32_C(0x0000a000);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[0] == 16u &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_PENDING_COLLISION &&
          fixture.fake_block.read.calls == 1u &&
          fixture.bridge.pending[0].active &&
          fixture.bridge.stats.guest_errors == 2u,
          "same-SP direct reentry did not return EBUSY before side effects");

    /* Four distinct stacks receive disjoint slots; the fifth gets EBUSY. */
    fixture_init();
    put_u32(TEST_L1, UINT32_C(0x00000802));
    fixture.cpu.cp15.dacr = 1u;
    for (i = 0u; i < TEST_BOUNCE_SLOTS; i++) {
        uint32_t uio = UINT32_C(0x00001000) + i * UINT32_C(0x100);
        uint32_t iov = UINT32_C(0x00002000) + i * UINT32_C(0x100);
        uint32_t user = UINT32_C(0x00005000) + i * UINT32_C(0x1000);
        put_uio_at(uio, iov, 1, i * 4u, 5u, 0u, 4, 1);
        put_u32(iov, user);
        put_u32(iov + 4u, 4u);
        fixture.cpu.r[0] = TEST_DEVICE;
        fixture.cpu.r[1] = uio;
        fixture.cpu.r[13] = UINT32_C(0x0000a000) + i * UINT32_C(0x100);
        fixture.cpu.r[14] = UINT32_C(0x00005001) + i * UINT32_C(0x100);
        CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
              fixture.cpu.r[0] ==
                  TEST_BOUNCE_PA + i * TEST_BOUNCE_STRIDE &&
              fixture.bridge.pending[i].active,
              "pending slot %u was not allocated disjointly", i);
    }
    put_uio_at(UINT32_C(0x00001400), UINT32_C(0x00002400),
               1, 0, 5u, 0u, 4, 1);
    put_u32(UINT32_C(0x00002400), UINT32_C(0x0000a000));
    put_u32(UINT32_C(0x00002404), 4u);
    fixture.cpu.r[0] = TEST_DEVICE;
    fixture.cpu.r[1] = UINT32_C(0x00001400);
    fixture.cpu.r[13] = UINT32_C(0x0000a400);
    fixture.cpu.r[14] = UINT32_C(0x00005401);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[0] == 16u &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_PENDING_EXHAUSTED &&
          fixture.bridge.stats.redirected_requests == TEST_BOUNCE_SLOTS &&
          fixture.bridge.stats.guest_errors == 1u,
          "fifth distinct continuation did not return bounded EBUSY");

    /* Two pending stacks can complete independently and out of order. */
    fixture_init();
    put_u32(TEST_L1, UINT32_C(0x00000802));
    fixture.cpu.cp15.dacr = 1u;
    for (i = 0u; i < 2u; i++) {
        uint32_t uio = UINT32_C(0x00001000) + i * UINT32_C(0x100);
        uint32_t iov = UINT32_C(0x00002000) + i * UINT32_C(0x100);
        put_uio_at(uio, iov, 1, 9 + i * 4u, 5u, 0u, 4, 1);
        put_u32(iov, UINT32_C(0x00005000) + i * UINT32_C(0x1000));
        put_u32(iov + 4u, 4u);
        fixture.cpu.r[0] = TEST_DEVICE;
        fixture.cpu.r[1] = uio;
        fixture.cpu.r[13] = UINT32_C(0x0000b000) + i * UINT32_C(0x100);
        fixture.cpu.r[14] = UINT32_C(0x00006001) + i * UINT32_C(0x100);
        CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED,
              "multi-pending setup %u failed", i);
    }
    fixture.cpu.r[13] = UINT32_C(0x0000b100);
    simulate_uiomove_at(UINT32_C(0x00001100), UINT32_C(0x00002100),
                       4u, 0u);
    CHECK(invoke_completion() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[15] == UINT32_C(0x00006100) &&
          get_u32(UINT32_C(0x00001100) + UIO_SEGFLG) == 5u &&
          fixture.bridge.pending[0].active &&
          !fixture.bridge.pending[1].active,
          "second pending stack did not complete independently");
    fixture.cpu.r[13] = UINT32_C(0x0000b000);
    fixture.cpu.r[14] = TEST_RAW_COMPLETION_PC | 1u;
    simulate_uiomove_at(UINT32_C(0x00001000), UINT32_C(0x00002000),
                       4u, 0u);
    CHECK(invoke_completion() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[15] == UINT32_C(0x00006000) &&
          !fixture.bridge.pending[0].active &&
          fixture.bridge.stats.redirected_completions == 2u,
          "first pending stack did not complete after out-of-order peer");
}

static void test_completion_failures_and_partial_errno(void) {
    const uint32_t l2 = UINT32_C(0x00017000);
    const uint32_t small_page_ap3 = UINT32_C(0x00000ff2);
    uint8_t expected[32];
    size_t i;

    fixture_init();
    CHECK(invoke_completion() == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_STALE_COMPLETION,
          "unmatched E4 completion did not fail closed");

    /*
     * If the kernel UIO mapping changes while native uiomove is blocked, the
     * saved PA may belong to an unrelated allocation. Halt without restoring
     * through that stale PA.
     */
    fixture_init();
    put_u32(TEST_L1, (l2 & UINT32_C(0xfffffc00)) | 1u);
    put_u32(l2 + 1u * 4u, UINT32_C(0x00001000) | small_page_ap3);
    put_u32(l2 + 4u * 4u, 0u); /* missing user page forces redirect */
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.r[13] = UINT32_C(0x0000bf00);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 7u,
          "completion-remap setup did not redirect");
    put_u32(UINT32_C(0x00003000) + UIO_SEGFLG,
            UINT32_C(0x11223344));
    put_u32(l2 + 1u * 4u, UINT32_C(0x00003000) | small_page_ap3);
    fixture.cpu.r[0] = 0u;
    CHECK(invoke_completion() == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_MALFORMED_COMPLETION &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 7u &&
          get_u32(UINT32_C(0x00003000) + UIO_SEGFLG) ==
              UINT32_C(0x11223344) &&
          !fixture.bridge.pending[0].active,
          "remapped completion restored through a stale physical address");

    /* A banked mode with the same numeric SP cannot steal a continuation. */
    fixture_init();
    put_u32(TEST_L1, UINT32_C(0x00000802));
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.r[13] = UINT32_C(0x0000be00);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED,
          "mode-key setup did not redirect");
    fixture.cpu.cpsr =
        (fixture.cpu.cpsr & ~ARM_CPSR_MODE_MASK) | ARM_MODE_IRQ;
    CHECK(invoke_completion() == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_STALE_COMPLETION &&
          fixture.bridge.pending[0].active,
          "same-SP completion from a different privileged mode was accepted");

    /* A direct jump from E3 to E4 leaves the bounce PA in r0: reject it. */
    fixture_init();
    put_u32(TEST_L1, UINT32_C(0x00000802));
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.r[13] = UINT32_C(0x0000bd00);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[0] == TEST_BOUNCE_PA,
          "forged-completion setup did not expose the bounce ABI");
    CHECK(invoke_completion() == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_MALFORMED_COMPLETION &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 5u &&
          !fixture.bridge.pending[0].active,
          "E4 accepted an impossible bounce-address errno");

    /* uiomove never changes direction; a flip must not write stale bounce. */
    fixture_init();
    put_uio(TEST_IOV, 1, 55, 5u, 1u, 32, 1);
    put_iov(0u, TEST_USER, 32u);
    put_u32(TEST_L1, UINT32_C(0x00000402));
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.r[13] = UINT32_C(0x0000bc00);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED,
          "direction-flip setup did not redirect");
    memset(fixture.ram.bytes + TEST_BOUNCE_PA, 0x6a, 32u);
    simulate_simple_uiomove(32u, 0u);
    put_u32(TEST_UIO + UIO_RW, 0u);
    CHECK(invoke_completion() == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_MALFORMED_COMPLETION &&
          fixture.fake_block.write.calls == 0u &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 5u &&
          !fixture.bridge.pending[0].active,
          "completion direction flip persisted stale bounce bytes");

    /* Residual progress without the identical offset progress is malformed. */
    fixture_init();
    put_u32(TEST_L1, UINT32_C(0x00000802));
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.r[13] = UINT32_C(0x0000c000);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED,
          "malformed-completion setup did not redirect");
    put_u32(TEST_UIO + UIO_RESID, 16u);
    fixture.cpu.r[0] = 14u;
    CHECK(invoke_completion() == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code ==
              MD_RAW_BRIDGE_ERROR_MALFORMED_COMPLETION &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 5u &&
          !fixture.bridge.pending[0].active,
          "inconsistent uiomove progress did not restore and fail closed");

    /*
     * A native WRITE may consume a prefix before returning EFAULT. Persist
     * exactly that in-media prefix, retain the errno, and restore the segment.
     */
    fixture_init();
    put_uio(TEST_IOV, 1, 101, 5u, 1u, 32, 1);
    put_iov(0u, TEST_USER, 32u);
    put_u32(TEST_L1, UINT32_C(0x00000402)); /* user no-access */
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.r[13] = UINT32_C(0x0000c100);
    fixture.cpu.r[14] = UINT32_C(0x00007201);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED,
          "partial-write setup did not redirect");
    for (i = 0u; i < sizeof expected; i++) {
        expected[i] = (uint8_t)(0x60u + i);
        fixture.ram.bytes[TEST_BOUNCE_PA + i] = expected[i];
    }
    simulate_simple_uiomove(12u, 14u);
    CHECK(invoke_completion() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[0] == 14u &&
          fixture.cpu.r[15] == UINT32_C(0x00007200) &&
          fixture.cpu.r[14] == UINT32_C(0x00007201) &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 5u &&
          memcmp(fixture.fake_block.bytes + 101u, expected, 12u) == 0,
          "partial uiomove errno did not persist its exact WRITE prefix");
    CHECK(fixture.bridge.stats.successful_writes == 0u &&
          fixture.bridge.stats.bytes_written == 12u &&
          fixture.bridge.stats.media_bytes_written == 12u &&
          fixture.bridge.stats.partial_uiomove_errors == 1u &&
          fixture.bridge.stats.guest_errors == 1u &&
          fixture.bridge.last_guest_error.code ==
              MD_RAW_BRIDGE_ERROR_UIOMOVE &&
          fixture.bridge.last_guest_error.guest_errno == 14,
          "partial uiomove errno statistics or diagnostic wrong");

    /* A partial EFAULT can span the media end into the coherent guard. */
    fixture_init();
    put_uio(TEST_IOV, 1, TEST_MEDIA_SIZE - 8u, 5u, 1u, 32, 1);
    put_iov(0u, TEST_USER, 32u);
    put_u32(TEST_L1, UINT32_C(0x00000402));
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.r[13] = UINT32_C(0x0000c200);
    fixture.cpu.r[14] = UINT32_C(0x00007301);
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED,
          "partial guard WRITE setup did not redirect");
    for (i = 0u; i < sizeof expected; i++) {
        expected[i] = (uint8_t)(0x90u + i);
        fixture.ram.bytes[TEST_BOUNCE_PA + i] = expected[i];
    }
    simulate_simple_uiomove(12u, 14u);
    CHECK(invoke_completion() == ARM_SVC_REDIRECTED &&
          fixture.cpu.r[0] == 14u &&
          memcmp(fixture.fake_block.bytes + TEST_MEDIA_SIZE - 8u,
                 expected, 8u) == 0 &&
          memcmp(fixture.bridge.guard_tail, expected + 8u, 4u) == 0 &&
          fixture.bridge.stats.media_bytes_written == 8u &&
          fixture.bridge.stats.guard_bytes_written == 4u &&
          fixture.bridge.stats.bytes_written == 12u,
          "partial EFAULT did not split its media and guard prefixes");

    /* A redirected READ staging failure has no guest/uio side effect. */
    fixture_init();
    put_u32(TEST_L1, UINT32_C(0x00000802));
    fixture.cpu.cp15.dacr = 1u;
    fixture.fake_block.read.mode = IO_ERROR;
    CHECK(invoke_entry_raw() == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_BLOCK_IO &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 5u &&
          get_u32(TEST_UIO + UIO_RESID) == 32u &&
          !fixture.bridge.pending[0].active,
          "redirected READ staging failure leaked pending/uio state");

    /*
     * A WRITE backend failure happens after native uiomove changed memory.
     * The bridge restores the segment and clears ownership, then halts instead
     * of pretending the already-advanced uio was persisted.
     */
    fixture_init();
    put_uio(TEST_IOV, 1, 19, 5u, 1u, 32, 1);
    put_iov(0u, TEST_USER, 32u);
    put_u32(TEST_L1, UINT32_C(0x00000402));
    fixture.cpu.cp15.dacr = 1u;
    CHECK(invoke_entry_raw() == ARM_SVC_REDIRECTED,
          "redirected WRITE backend-failure setup did not redirect");
    memset(fixture.ram.bytes + TEST_BOUNCE_PA, 0x6d, 32u);
    simulate_simple_uiomove(32u, 0u);
    fixture.fake_block.write.mode = IO_ERROR;
    CHECK(invoke_completion() == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code == MD_RAW_BRIDGE_ERROR_BLOCK_IO &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 5u &&
          get_u32(TEST_UIO + UIO_RESID) == 0u &&
          !fixture.bridge.pending[0].active,
          "post-uiomove WRITE backend failure did not restore then halt");
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
    config.site.encoding = UINT32_C(0xdfe2);
    CHECK(!md_raw_bridge_config_valid(&config),
          "wrong entry SVC immediate accepted");
    config = fixture.bridge.config;
    config.completion_site.pc++;
    CHECK(!md_raw_bridge_config_valid(&config),
          "non-adjacent completion site accepted");
    config = fixture.bridge.config;
    config.completion_site.encoding = UINT32_C(0xdfe5);
    CHECK(!md_raw_bridge_config_valid(&config),
          "wrong completion SVC immediate accepted");
    config = fixture.bridge.config;
    config.uiomove_thumb_pc |= 1u;
    CHECK(!md_raw_bridge_config_valid(&config),
          "odd uiomove fetch PC accepted");
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
    config = fixture.bridge.config;
    config.bounce_slot_count = 0u;
    CHECK(!md_raw_bridge_config_valid(&config),
          "zero bounce slots accepted");
    config = fixture.bridge.config;
    config.bounce_slot_count = MD_RAW_BRIDGE_MAX_BOUNCE_SLOTS + 1u;
    CHECK(!md_raw_bridge_config_valid(&config),
          "too many bounce slots accepted");
    config = fixture.bridge.config;
    config.bounce_stride = MD_RAW_BRIDGE_MAX_TRANSFER - 1u;
    CHECK(!md_raw_bridge_config_valid(&config),
          "undersized bounce slot accepted");
    config = fixture.bridge.config;
    config.bounce_base_pa++;
    CHECK(!md_raw_bridge_config_valid(&config),
          "unaligned bounce base accepted");
    config = fixture.bridge.config;
    config.bounce_base_pa = TEST_RAM_SIZE - MD_RAW_BRIDGE_MAX_TRANSFER;
    CHECK(!md_raw_bridge_config_valid(&config),
          "bounce reservation extending beyond RAM accepted");
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

static void test_exact_svc_pair_redirect_and_halt_rollback(void) {
    arm_cpu_t before;
    arm_cpu_t normalized;
    arm_status_t status;

    fixture_init();
    put_uio(TEST_IOV, 1, 0, 5u, 0u, 4, 1);
    put_iov(0u, TEST_USER, 4u);
    put_u32(TEST_RAW_PC, UINT32_C(0xdfe4dfe3));
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
          fixture.cpu.r[15] == UINT32_C(0x00003500) &&
          fixture.cpu.cycles == before.cycles + 1u &&
          memcmp(&normalized, &before, sizeof before) == 0,
          "direct SVC did not redirect past completion to the saved caller");

    /*
     * Exercise both real Thumb decodes: E3 redirects to uiomove, and the
     * simulated native return fetches E4, which restores LR and redirects to
     * the original caller. This catches sequential-PC retirement regressions.
     */
    fixture_init();
    put_u32(TEST_RAW_PC, UINT32_C(0xdfe4dfe3));
    put_u32(TEST_L1, UINT32_C(0x00000802));
    fixture.cpu.cp15.dacr = 1u;
    fixture.cpu.r[13] = UINT32_C(0x0000d000);
    fixture.cpu.r[14] = UINT32_C(0x00007501);
    arm_bus_set_privileged_svc_handler(&fixture.bus,
                                       md_raw_bridge_handle_svc,
                                       &fixture.bridge);
    fixture.cpu.r[15] = TEST_RAW_PC;
    before = fixture.cpu;
    status = arm_step(&fixture.cpu);
    CHECK(status == ARM_OK &&
          fixture.cpu.r[15] == TEST_UIOMOVE_PC &&
          fixture.cpu.r[14] == (TEST_RAW_COMPLETION_PC | 1u) &&
          fixture.cpu.cycles == before.cycles + 1u &&
          fixture.bridge.pending[0].active,
          "decoded E3 did not redirect into native uiomove");
    memcpy(fixture.ram.bytes + TEST_USER,
           fixture.ram.bytes + TEST_BOUNCE_PA, 32u);
    simulate_simple_uiomove(32u, 0u);
    fixture.cpu.r[15] = TEST_RAW_COMPLETION_PC;
    status = arm_step(&fixture.cpu);
    CHECK(status == ARM_OK &&
          fixture.cpu.r[15] == UINT32_C(0x00007500) &&
          fixture.cpu.r[14] == UINT32_C(0x00007501) &&
          fixture.cpu.r[0] == 0u &&
          fixture.cpu.cycles == before.cycles + 2u &&
          get_u32(TEST_UIO + UIO_SEGFLG) == 5u &&
          !fixture.bridge.pending[0].active,
          "decoded E4 did not complete and redirect to the saved caller");

    fixture_init();
    put_uio(TEST_IOV, 1, 0, 5u, 0u, 4, 1);
    put_iov(0u, TEST_USER, 4u);
    put_u32(TEST_RAW_PC, UINT32_C(0xdfe4dfe3));
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
    test_media_eof_semantics();
    test_uiomove_redirect_and_completion();
    test_uiomove_segment_conversion();
    test_nonzero_ram_base_redirect();
    test_all_user_segment_variants();
    test_page_split_and_max_iovec_bound();
    test_ttbr_split_kernel_metadata_and_user_buffer();
    test_legacy_subpage_permissions();
    test_guest_errors_are_atomic();
    test_malformed_uio_fails_closed();
    test_faultable_plan_scans_later_spans();
    test_pending_slots_collision_and_exhaustion();
    test_completion_failures_and_partial_errno();
    test_backend_failures_do_not_commit_uio();
    test_exact_gate_and_configuration();
    test_exact_svc_pair_redirect_and_halt_rollback();
    test_saturating_diagnostics_and_strings();
    printf("\n%u passed, %u failed\n", g_passes, g_failures);
    return g_failures == 0u ? 0 : 1;
}

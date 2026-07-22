/*
 * iOS3-VM -- adversarial tests for the host-backed memory-disk bridge.
 *
 * The bridge is a privileged boundary.  These tests intentionally feed it
 * malformed firmware sites, CPU state, 64-bit bcopy arguments, translations,
 * backend results, and cancellation timing; accepting an invalid request is a
 * VM integrity bug, while turning an I/O failure into an ordinary guest SVC
 * can corrupt the root filesystem.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "md_bridge.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_RAM_SIZE UINT32_C(65536)
#define TEST_MEDIA_CAPACITY UINT32_C(8192)
#define TEST_READ_PC UINT32_C(0x100)
#define TEST_WRITE_PC UINT32_C(0x110)
#define TEST_READ_SVC UINT32_C(0xdf41)
#define TEST_WRITE_SVC UINT32_C(0xdf42)
#define TEST_TOKEN UINT64_C(0x80000000)
#define TEST_SP UINT32_C(0x200)
#define TEST_GUEST UINT32_C(0x1000)

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
    uint64_t read8_calls;
    uint64_t read32_calls;
    uint64_t write8_calls;
} fake_ram_t;

static bool ram_resolve(const fake_ram_t *ram, uint32_t address,
                        size_t length, size_t *offset) {
    uint64_t start = address;
    uint64_t end = start + length;
    uint64_t ram_end = (uint64_t)ram->base + TEST_RAM_SIZE;

    if (start < ram->base || end > ram_end)
        return false;
    *offset = (size_t)(start - ram->base);
    return true;
}

static uint8_t ram_read8(void *context, uint32_t address) {
    fake_ram_t *ram = (fake_ram_t *)context;
    size_t offset;
    ram->read8_calls++;
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
    ram->write8_calls++;
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

static arm_bus_t make_bus(fake_ram_t *ram) {
    arm_bus_t bus = {0};
    bus.ctx = ram;
    bus.read32 = ram_read32;
    bus.read16 = ram_read16;
    bus.read8 = ram_read8;
    bus.write32 = ram_write32;
    bus.write16 = ram_write16;
    bus.write8 = ram_write8;
    return bus;
}

typedef enum {
    FAKE_NORMAL = 0,
    FAKE_ERROR,
    FAKE_PARTIAL_THEN_ERROR,
    FAKE_ALWAYS_RETRY,
    FAKE_ALWAYS_ZERO,
    FAKE_OVERREPORT,
    FAKE_RETRY_WITH_DATA,
    FAKE_ERROR_WITH_DATA,
    FAKE_INVALID_STATUS
} fake_io_mode_t;

typedef struct {
    fake_io_mode_t mode;
    unsigned calls;
    unsigned retries_left;
    size_t max_chunk;
    size_t first_chunk;
} fake_behavior_t;

typedef struct {
    uint8_t bytes[TEST_MEDIA_CAPACITY];
    size_t size;
    fake_behavior_t read;
    fake_behavior_t write;
    unsigned flush_calls;
} fake_block_t;

static bool fake_special(fake_behavior_t *behavior, void *destination,
                         const void *source, size_t requested, size_t *actual,
                         vm_block_io_status_t *status) {
    behavior->calls++;
    *actual = 0u;
    switch (behavior->mode) {
    case FAKE_NORMAL:
        if (behavior->retries_left != 0u) {
            behavior->retries_left--;
            *status = VM_BLOCK_IO_RETRY;
            return true;
        }
        return false;
    case FAKE_ERROR:
        *status = VM_BLOCK_IO_ERROR;
        return true;
    case FAKE_PARTIAL_THEN_ERROR:
        if (behavior->calls != 1u) {
            *status = VM_BLOCK_IO_ERROR;
            return true;
        }
        return false;
    case FAKE_ALWAYS_RETRY:
        *status = VM_BLOCK_IO_RETRY;
        return true;
    case FAKE_ALWAYS_ZERO:
        *status = VM_BLOCK_IO_OK;
        return true;
    case FAKE_OVERREPORT:
        *actual = requested + 1u;
        *status = VM_BLOCK_IO_OK;
        return true;
    case FAKE_RETRY_WITH_DATA:
    case FAKE_ERROR_WITH_DATA:
        if (requested != 0u) {
            *actual = 1u;
            if (destination != NULL && source != NULL)
                memcpy(destination, source, 1u);
        }
        *status = behavior->mode == FAKE_RETRY_WITH_DATA
            ? VM_BLOCK_IO_RETRY : VM_BLOCK_IO_ERROR;
        return true;
    case FAKE_INVALID_STATUS:
        *status = (vm_block_io_status_t)99;
        return true;
    default:
        *status = VM_BLOCK_IO_ERROR;
        return true;
    }
}

static size_t fake_count(const fake_behavior_t *behavior, size_t requested,
                         size_t available) {
    size_t count = requested < available ? requested : available;
    if (behavior->calls == 1u && behavior->first_chunk != 0u &&
        count > behavior->first_chunk)
        count = behavior->first_chunk;
    if (behavior->max_chunk != 0u && count > behavior->max_chunk)
        count = behavior->max_chunk;
    return count;
}

static vm_block_io_status_t fake_read_at(void *context, uint64_t offset,
                                         void *destination, size_t requested,
                                         size_t *actual) {
    fake_block_t *fake = (fake_block_t *)context;
    vm_block_io_status_t status = VM_BLOCK_IO_OK;
    size_t available;
    size_t count;
    const void *protocol_source = fake->bytes;

    if (offset < fake->size)
        protocol_source = fake->bytes + (size_t)offset;
    if (fake_special(&fake->read, destination, protocol_source, requested,
                     actual, &status))
        return status;
    if (offset >= fake->size)
        return VM_BLOCK_IO_OK;
    available = fake->size - (size_t)offset;
    count = fake_count(&fake->read, requested, available);
    memcpy(destination, fake->bytes + (size_t)offset, count);
    *actual = count;
    return VM_BLOCK_IO_OK;
}

static vm_block_io_status_t fake_write_at(void *context, uint64_t offset,
                                          const void *source, size_t requested,
                                          size_t *actual) {
    fake_block_t *fake = (fake_block_t *)context;
    vm_block_io_status_t status = VM_BLOCK_IO_OK;
    size_t available;
    size_t count;
    void *protocol_destination = fake->bytes;

    if (offset < fake->size)
        protocol_destination = fake->bytes + (size_t)offset;
    if (fake_special(&fake->write, protocol_destination, source, requested,
                     actual, &status))
        return status;
    if (offset >= fake->size)
        return VM_BLOCK_IO_OK;
    available = fake->size - (size_t)offset;
    count = fake_count(&fake->write, requested, available);
    memcpy(fake->bytes + (size_t)offset, source, count);
    *actual = count;
    return VM_BLOCK_IO_OK;
}

static vm_block_io_status_t fake_flush(void *context) {
    fake_block_t *fake = (fake_block_t *)context;
    fake->flush_calls++;
    return VM_BLOCK_IO_OK;
}

typedef struct {
    unsigned checks;
    unsigned cancel_at;
} fake_cancel_t;

static bool fake_cancel(void *context) {
    fake_cancel_t *cancel = (fake_cancel_t *)context;
    bool result = cancel->checks >= cancel->cancel_at;
    cancel->checks++;
    return result;
}

typedef struct {
    fake_ram_t ram;
    arm_bus_t bus;
    fake_block_t fake_block;
    vm_block_t block;
    md_bridge_t bridge;
    arm_cpu_t cpu;
} fixture_t;

static md_bridge_config_t default_config(fixture_t *fixture) {
    md_bridge_config_t config = {0};
    config.read_site.pc = TEST_READ_PC;
    config.read_site.encoding = TEST_READ_SVC;
    config.write_site.pc = TEST_WRITE_PC;
    config.write_site.encoding = TEST_WRITE_SVC;
    config.token_base = TEST_TOKEN;
    config.media_size = TEST_MEDIA_CAPACITY;
    config.ram_base = 0u;
    config.ram_size = TEST_RAM_SIZE;
    config.ram = fixture->ram.bytes;
    config.block = &fixture->block;
    return config;
}

static void fixture_init(fixture_t *fixture) {
    md_bridge_config_t config;
    size_t i;

    *fixture = (fixture_t){0};
    fixture->bus = make_bus(&fixture->ram);
    fixture->fake_block.size = TEST_MEDIA_CAPACITY;
    for (i = 0u; i < fixture->fake_block.size; i++)
        fixture->fake_block.bytes[i] = (uint8_t)(i ^ (i >> 8));
    fixture->block.context = &fixture->fake_block;
    fixture->block.size = TEST_MEDIA_CAPACITY;
    fixture->block.identity = 1u;
    fixture->block.generation = 1u;
    fixture->block.read_at = fake_read_at;
    fixture->block.write_at = fake_write_at;
    fixture->block.flush = fake_flush;
    config = default_config(fixture);
    md_bridge_init(&fixture->bridge, &config);
    arm_reset(&fixture->cpu, &fixture->bus);
    fixture->cpu.cpsr = ARM_MODE_SVC | ARM_CPSR_T;
    fixture->cpu.r[13] = TEST_SP;
    fixture->cpu.r[15] = TEST_READ_PC;
}

static void put_u32(fake_ram_t *ram, uint32_t address, uint32_t value) {
    size_t offset;

    if (ram_resolve(ram, address, 4u, &offset)) {
        ram->bytes[offset] = (uint8_t)value;
        ram->bytes[offset + 1u] = (uint8_t)(value >> 8);
        ram->bytes[offset + 2u] = (uint8_t)(value >> 16);
        ram->bytes[offset + 3u] = (uint8_t)(value >> 24);
    }
}

static void set_request(fixture_t *fixture, md_bridge_direction_t direction,
                        uint64_t guest, uint64_t token, uint32_t length) {
    uint64_t source = direction == MD_BRIDGE_DIRECTION_READ ? token : guest;
    uint64_t destination = direction == MD_BRIDGE_DIRECTION_READ ? guest : token;
    fixture->cpu.r[0] = (uint32_t)source;
    fixture->cpu.r[1] = (uint32_t)(source >> 32);
    fixture->cpu.r[2] = (uint32_t)destination;
    fixture->cpu.r[3] = (uint32_t)(destination >> 32);
    put_u32(&fixture->ram, fixture->cpu.r[13], length);
}

static arm_svc_result_t invoke(fixture_t *fixture,
                               md_bridge_direction_t direction) {
    uint32_t pc = direction == MD_BRIDGE_DIRECTION_READ
        ? TEST_READ_PC : TEST_WRITE_PC;
    uint32_t encoding = direction == MD_BRIDGE_DIRECTION_READ
        ? TEST_READ_SVC : TEST_WRITE_SVC;
    return md_bridge_handle_svc(&fixture->bridge, &fixture->cpu,
                                pc, encoding);
}

static void expect_error(fixture_t *fixture,
                         md_bridge_direction_t direction,
                         md_bridge_error_code_t code,
                         const char *label) {
    arm_svc_result_t result = invoke(fixture, direction);
    CHECK(result == ARM_SVC_ERROR, "%s did not fail closed (%d)",
          label, (int)result);
    CHECK(fixture->bridge.last_error.code == code,
          "%s error %d, expected %d", label,
          (int)fixture->bridge.last_error.code, (int)code);
}

static void test_success_and_cpu_immutability(void) {
    fixture_t fixture;
    arm_cpu_t before;
    uint8_t source_copy[32];
    size_t i;

    fixture_init(&fixture);
    memset(fixture.ram.bytes + TEST_GUEST, 0xa5, sizeof(source_copy));
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN + 13u, (uint32_t)sizeof(source_copy));
    before = fixture.cpu;
    memcpy(source_copy, fixture.fake_block.bytes + 13u, sizeof(source_copy));
    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_READ) == ARM_SVC_HANDLED,
          "valid read service was not handled");
    CHECK(memcmp(fixture.ram.bytes + TEST_GUEST, source_copy,
                 sizeof(source_copy)) == 0, "read copied wrong bytes");
    CHECK(memcmp(&fixture.cpu, &before, sizeof(before)) == 0,
          "read service changed CPU registers at an LR-dead site");
    CHECK(fixture.bridge.stats.successful_reads == 1u &&
          fixture.bridge.stats.bytes_read == sizeof(source_copy) &&
          fixture.bridge.stats.successful_writes == 0u &&
          fixture.bridge.stats.failures == 0u,
          "read statistics wrong");
    CHECK(fixture.fake_block.flush_calls == 0u,
          "bridge flushed per transfer");
    CHECK(fixture.ram.read8_calls == 0u &&
          fixture.ram.read32_calls == 0u &&
          fixture.ram.write8_calls == 0u,
          "read bulk path used scalar bus callbacks");

    for (i = 0u; i < sizeof(source_copy); i++)
        fixture.ram.bytes[TEST_GUEST + i] = (uint8_t)(0xe0u + i);
    memcpy(source_copy, fixture.ram.bytes + TEST_GUEST, sizeof(source_copy));
    set_request(&fixture, MD_BRIDGE_DIRECTION_WRITE, TEST_GUEST,
                TEST_TOKEN + 91u, (uint32_t)sizeof(source_copy));
    before = fixture.cpu;
    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_WRITE) == ARM_SVC_HANDLED,
          "valid write service was not handled");
    CHECK(memcmp(fixture.fake_block.bytes + 91u, source_copy,
                 sizeof(source_copy)) == 0, "write copied wrong bytes");
    CHECK(memcmp(fixture.ram.bytes + TEST_GUEST, source_copy,
                 sizeof(source_copy)) == 0, "write mutated guest source");
    CHECK(memcmp(&fixture.cpu, &before, sizeof(before)) == 0,
          "write service changed CPU registers at an LR-dead site");
    CHECK(fixture.bridge.stats.successful_writes == 1u &&
          fixture.bridge.stats.bytes_written == sizeof(source_copy),
          "write statistics wrong");
    CHECK(fixture.fake_block.flush_calls == 0u,
          "write unexpectedly flushed media");
    CHECK(fixture.ram.read8_calls == 0u &&
          fixture.ram.read32_calls == 0u &&
          fixture.ram.write8_calls == 0u,
          "write bulk path used scalar bus callbacks");
}

static void test_exact_gate_is_unhandled(void) {
    fixture_t fixture;
    md_bridge_stats_t stats;
    md_bridge_error_t error;

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    stats = fixture.bridge.stats;
    error = fixture.bridge.last_error;
    fixture.cpu.r[14] = UINT32_C(0x55aa33cc);

    fixture.cpu.cpsr = ARM_MODE_USR | ARM_CPSR_T;
    CHECK(md_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                               TEST_READ_PC, TEST_READ_SVC) ==
          ARM_SVC_UNHANDLED, "user-mode service reached host bridge");
    fixture.cpu.cpsr = ARM_MODE_SVC;
    CHECK(md_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                               TEST_READ_PC, TEST_READ_SVC) ==
          ARM_SVC_UNHANDLED, "A32 state reached Thumb bridge");
    fixture.cpu.cpsr = ARM_MODE_SVC | ARM_CPSR_T;
    CHECK(md_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                               TEST_READ_PC + 2u, TEST_READ_SVC) ==
          ARM_SVC_UNHANDLED, "wrong PC was recognized");
    CHECK(md_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                               TEST_READ_PC, TEST_READ_SVC ^ 1u) ==
          ARM_SVC_UNHANDLED, "wrong SVC immediate was recognized");
    CHECK(md_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                               TEST_READ_PC,
                               TEST_READ_SVC | UINT32_C(0x10000)) ==
          ARM_SVC_UNHANDLED, "non-raw Thumb encoding was recognized");
    CHECK(fixture.cpu.r[14] == UINT32_C(0x55aa33cc),
          "unhandled service changed link register");
    CHECK(memcmp(&fixture.bridge.stats, &stats, sizeof(stats)) == 0 &&
          memcmp(&fixture.bridge.last_error, &error, sizeof(error)) == 0,
          "unhandled calls changed diagnostics");
}

static void test_configuration_preflight(void) {
    fixture_t fixture;
    md_bridge_config_t config;

    fixture_init(&fixture);
    config = fixture.bridge.config;
    CHECK(md_bridge_config_valid(&config),
          "known-good bridge configuration failed public preflight");
    config.token_base = config.ram_base;
    CHECK(!md_bridge_config_valid(&config),
          "overlapping media/RAM apertures passed public preflight");
    CHECK(!md_bridge_config_valid(NULL),
          "NULL bridge configuration passed public preflight");
}

static void test_mode_and_configuration_fail_closed(void) {
    fixture_t fixture;
    md_bridge_config_t config;
    vm_block_t alternate_block;

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    fixture.cpu.cpsr = ARM_CPSR_T;
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_INVALID_MODE, "invalid CPSR mode");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    config = fixture.bridge.config;
    config.block = NULL;
    md_bridge_init(&fixture.bridge, &config);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_INVALID_CONFIG, "null block");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    config = fixture.bridge.config;
    config.ram = NULL;
    md_bridge_init(&fixture.bridge, &config);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_INVALID_CONFIG, "null RAM backing");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    config = fixture.bridge.config;
    alternate_block = fixture.block;
    alternate_block.size--;
    config.block = &alternate_block;
    md_bridge_init(&fixture.bridge, &config);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_INVALID_CONFIG, "block size mismatch");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    fixture.block.read_at = NULL;
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_INVALID_CONFIG, "missing block read");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_WRITE, TEST_GUEST,
                TEST_TOKEN, 1u);
    fixture.block.write_at = NULL;
    expect_error(&fixture, MD_BRIDGE_DIRECTION_WRITE,
                 MD_BRIDGE_ERROR_INVALID_CONFIG, "missing block write");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    config = fixture.bridge.config;
    config.media_size = 0u;
    fixture.block.size = 0u;
    md_bridge_init(&fixture.bridge, &config);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_INVALID_CONFIG, "empty media aperture");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    config = fixture.bridge.config;
    config.token_base = 0u;
    md_bridge_init(&fixture.bridge, &config);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_INVALID_CONFIG, "overlapping apertures");

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.token_base = UINT64_C(0xfffff000);
    config.media_size = UINT64_C(8192);
    fixture.block.size = config.media_size;
    md_bridge_init(&fixture.bridge, &config);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                config.token_base, 1u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_INVALID_CONFIG, "32-bit token overflow");

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.ram_base = UINT64_C(0xfffff000);
    config.ram_size = UINT64_C(8192);
    md_bridge_init(&fixture.bridge, &config);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_INVALID_CONFIG, "32-bit RAM overflow");

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.token_base++;
    md_bridge_init(&fixture.bridge, &config);
    CHECK(md_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                               config.read_site.pc,
                               config.read_site.encoding) == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code == MD_BRIDGE_ERROR_INVALID_CONFIG,
          "misaligned token base was accepted");

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.media_size++;
    fixture.block.size = config.media_size;
    md_bridge_init(&fixture.bridge, &config);
    CHECK(md_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                               config.read_site.pc,
                               config.read_site.encoding) == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code == MD_BRIDGE_ERROR_INVALID_CONFIG,
          "misaligned media size was accepted");

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.ram_base++;
    md_bridge_init(&fixture.bridge, &config);
    CHECK(md_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                               config.read_site.pc,
                               config.read_site.encoding) == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code == MD_BRIDGE_ERROR_INVALID_CONFIG,
          "misaligned RAM base was accepted");

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.ram_size--;
    md_bridge_init(&fixture.bridge, &config);
    CHECK(md_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                               config.read_site.pc,
                               config.read_site.encoding) == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code == MD_BRIDGE_ERROR_INVALID_CONFIG,
          "misaligned RAM size was accepted");

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.write_site.pc = config.read_site.pc + 2u;
    md_bridge_init(&fixture.bridge, &config);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_INVALID_CONFIG, "colliding BL sites");

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.write_site.encoding = config.read_site.encoding;
    md_bridge_init(&fixture.bridge, &config);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_INVALID_CONFIG, "duplicate SVC encodings");

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.read_site.pc = UINT32_C(0xfffffffe);
    md_bridge_init(&fixture.bridge, &config);
    CHECK(md_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                               config.read_site.pc,
                               config.read_site.encoding) == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code == MD_BRIDGE_ERROR_INVALID_CONFIG,
          "wrapping BL site was accepted");

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.read_site.pc |= 1u;
    md_bridge_init(&fixture.bridge, &config);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    CHECK(md_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                               config.read_site.pc,
                               config.read_site.encoding) == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code == MD_BRIDGE_ERROR_INVALID_CONFIG,
          "misaligned configured PC did not fail closed");

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.read_site.encoding = UINT32_C(0xbe41);
    md_bridge_init(&fixture.bridge, &config);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    CHECK(md_bridge_handle_svc(&fixture.bridge, &fixture.cpu,
                               config.read_site.pc,
                               config.read_site.encoding) == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code == MD_BRIDGE_ERROR_INVALID_CONFIG,
          "malformed configured encoding did not fail closed");
}

static void test_stack_validation(void) {
    fixture_t fixture;

    fixture_init(&fixture);
    fixture.cpu.r[13] = 2u;
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_STACK_ALIGNMENT, "unaligned SP");

    fixture_init(&fixture);
    fixture.cpu.r[13] = UINT32_C(0xfff);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_STACK_PAGE, "cross-page stack word");

    fixture_init(&fixture);
    fixture.cpu.r[13] = TEST_RAM_SIZE;
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_STACK_RANGE, "stack outside RAM");

    fixture_init(&fixture);
    fixture.cpu.cp15.sctlr |= ARM_SCTLR_M;
    fixture.cpu.cp15.ttbr0 = 0u;
    fixture.cpu.cp15.ttbcr = 0u;
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_STACK_TRANSLATION, "unmapped stack");
    CHECK(fixture.bridge.last_error.mmu_status != 0u,
          "translation failure did not preserve FSR");
    CHECK(fixture.ram.read32_calls != 0u &&
          fixture.ram.read8_calls == 0u && fixture.ram.write8_calls == 0u,
          "MMU fault did not use only the page-walk callback");

    fixture_init(&fixture);
    fixture.cpu.cp15.sctlr |= ARM_SCTLR_M;
    fixture.cpu.bus = NULL;
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_MISSING_BUS_ACCESS, "missing MMU bus");

    fixture_init(&fixture);
    fixture.cpu.cp15.sctlr |= ARM_SCTLR_M;
    fixture.bus.read32 = NULL;
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_MISSING_BUS_ACCESS, "missing MMU read32");
}

static void test_mmu_stack_and_direct_bulk_ram(void) {
    fixture_t fixture;

    fixture_init(&fixture);
    /* One manager-domain section maps stack VA 0x200 to physical RAM 0x200. */
    put_u32(&fixture.ram, UINT32_C(0x4000), UINT32_C(0x00000c02));
    fixture.cpu.cp15.ttbr0 = UINT32_C(0x4000);
    fixture.cpu.cp15.ttbcr = 0u;
    fixture.cpu.cp15.dacr = 3u;
    fixture.cpu.cp15.sctlr |= ARM_SCTLR_M;
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 32u);
    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_READ) == ARM_SVC_HANDLED,
          "mapped virtual stack service failed");
    CHECK(fixture.ram.read32_calls == 1u &&
          fixture.ram.read8_calls == 0u && fixture.ram.write8_calls == 0u,
          "bulk copy used scalar bus traffic beyond one MMU table walk");
    CHECK(memcmp(fixture.ram.bytes + TEST_GUEST,
                 fixture.fake_block.bytes, 32u) == 0,
          "MMU-stack read copied wrong bytes");
}

static void test_real_ram_base_translation_and_bulk_offsets(void) {
    const uint32_t real_ram_base = UINT32_C(0x08000000);
    const uint32_t stack_va = UINT32_C(0xc1000200);
    const uint32_t ttbr0 = real_ram_base + UINT32_C(0x4000);
    const uint32_t l1_address = ttbr0 + ((stack_va >> 20) << 2);
    fixture_t fixture;
    md_bridge_config_t config;
    arm_cpu_t before;

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ,
                (uint64_t)real_ram_base + TEST_GUEST,
                TEST_TOKEN + 23u, 32u);

    config = fixture.bridge.config;
    config.ram_base = real_ram_base;
    md_bridge_init(&fixture.bridge, &config);
    fixture.ram.base = real_ram_base;

    /* Map the kernel stack section at c1000000 to DRAM at 08000000. */
    put_u32(&fixture.ram, l1_address, real_ram_base | UINT32_C(0x0c02));
    fixture.cpu.r[13] = stack_va;
    fixture.cpu.cp15.ttbr0 = ttbr0;
    fixture.cpu.cp15.ttbcr = 0u;
    fixture.cpu.cp15.dacr = 3u;
    fixture.cpu.cp15.sctlr |= ARM_SCTLR_M;
    before = fixture.cpu;

    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_READ) == ARM_SVC_HANDLED,
          "real 0x08000000 RAM-base service failed");
    CHECK(memcmp(fixture.ram.bytes + TEST_GUEST,
                 fixture.fake_block.bytes + 23u, 32u) == 0,
          "nonzero RAM base resolved the bulk destination incorrectly");
    CHECK(fixture.ram.read32_calls == 1u &&
          fixture.ram.read8_calls == 0u && fixture.ram.write8_calls == 0u,
          "nonzero RAM-base bulk transfer escaped the direct byte aperture");
    CHECK(memcmp(&fixture.cpu, &before, sizeof(before)) == 0,
          "translated nonzero-base service changed CPU state");
}

static void test_argument_and_range_validation(void) {
    fixture_t fixture;

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, MD_BRIDGE_MAX_TRANSFER);
    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_READ) == ARM_SVC_HANDLED &&
          memcmp(fixture.ram.bytes + TEST_GUEST,
                 fixture.fake_block.bytes, MD_BRIDGE_MAX_TRANSFER) == 0,
          "exact 4096-byte transfer was rejected");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    fixture.cpu.r[1] = 1u;
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_ADDRESS_HIGH, "source high word");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    fixture.cpu.r[3] = 1u;
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_ADDRESS_HIGH, "destination high word");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 0u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_LENGTH, "zero length");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, MD_BRIDGE_MAX_TRANSFER + 1u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_LENGTH, "4097-byte length");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN - 1u, 1u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_TOKEN_RANGE, "token before aperture");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN + TEST_MEDIA_CAPACITY, 1u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_TOKEN_RANGE, "token at exclusive end");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_RAM_SIZE,
                TEST_TOKEN, 1u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_GUEST_RANGE, "guest at exclusive end");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN + UINT64_C(0xfff), 2u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_TOKEN_PAGE, "token page crossing");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, UINT64_C(0x1fff),
                TEST_TOKEN, 2u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_GUEST_PAGE, "guest page crossing");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_WRITE, TEST_GUEST,
                TEST_TOKEN + TEST_MEDIA_CAPACITY - 2u, 3u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_WRITE,
                 MD_BRIDGE_ERROR_TOKEN_RANGE, "token length overflow");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_WRITE,
                TEST_RAM_SIZE - 2u, TEST_TOKEN, 3u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_WRITE,
                 MD_BRIDGE_ERROR_GUEST_RANGE, "guest length overflow");
}

static void test_partial_retry_and_cancellation(void) {
    fixture_t fixture;
    fake_cancel_t cancel;
    md_bridge_config_t config;
    uint8_t expected[17];

    fixture_init(&fixture);
    fixture.fake_block.read.retries_left = 3u;
    fixture.fake_block.read.max_chunk = 2u;
    memcpy(expected, fixture.fake_block.bytes + 5u, sizeof(expected));
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN + 5u, (uint32_t)sizeof(expected));
    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_READ) == ARM_SVC_HANDLED,
          "partial/retry read did not complete");
    CHECK(fixture.fake_block.read.calls > 4u &&
          memcmp(fixture.ram.bytes + TEST_GUEST, expected,
                 sizeof(expected)) == 0,
          "partial/retry read data wrong");

    fixture_init(&fixture);
    fixture.fake_block.write.retries_left = 2u;
    fixture.fake_block.write.max_chunk = 3u;
    memset(fixture.ram.bytes + TEST_GUEST, 0x6c, sizeof(expected));
    set_request(&fixture, MD_BRIDGE_DIRECTION_WRITE, TEST_GUEST,
                TEST_TOKEN + 7u, (uint32_t)sizeof(expected));
    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_WRITE) == ARM_SVC_HANDLED &&
          memcmp(fixture.fake_block.bytes + 7u,
                 fixture.ram.bytes + TEST_GUEST, sizeof(expected)) == 0,
          "partial/retry write did not complete correctly");

    fixture_init(&fixture);
    cancel.checks = 0u;
    cancel.cancel_at = 0u;
    config = fixture.bridge.config;
    config.cancelled = fake_cancel;
    config.cancel_context = &cancel;
    md_bridge_init(&fixture.bridge, &config);
    memset(fixture.ram.bytes + TEST_GUEST, 0xa7, sizeof(expected));
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, (uint32_t)sizeof(expected));
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_BLOCK_IO, "cancelled read");
    CHECK(fixture.bridge.last_error.block_status == VM_BLOCK_STATUS_CANCELLED &&
          fixture.fake_block.read.calls == 0u &&
          fixture.bridge.last_error.transferred == 0u,
          "read cancellation diagnostics wrong");
    CHECK(fixture.ram.bytes[TEST_GUEST] == 0xa7,
          "cancelled read changed guest RAM");

    fixture_init(&fixture);
    cancel.checks = 0u;
    cancel.cancel_at = 1u;
    config = fixture.bridge.config;
    config.cancelled = fake_cancel;
    config.cancel_context = &cancel;
    md_bridge_init(&fixture.bridge, &config);
    fixture.fake_block.write.max_chunk = 2u;
    memset(fixture.ram.bytes + TEST_GUEST, 0x55, sizeof(expected));
    set_request(&fixture, MD_BRIDGE_DIRECTION_WRITE, TEST_GUEST,
                TEST_TOKEN, (uint32_t)sizeof(expected));
    expect_error(&fixture, MD_BRIDGE_DIRECTION_WRITE,
                 MD_BRIDGE_ERROR_BLOCK_IO, "cancelled partial write");
    CHECK(fixture.bridge.last_error.block_status == VM_BLOCK_STATUS_CANCELLED &&
          fixture.bridge.last_error.transferred == 2u &&
          fixture.fake_block.bytes[0] == 0x55 &&
          fixture.fake_block.bytes[1] == 0x55,
          "partial-write cancellation did not report committed prefix");
}

static void test_backend_failures_and_atomicity(void) {
    static const fake_io_mode_t failure_modes[] = {
        FAKE_ERROR, FAKE_ALWAYS_RETRY, FAKE_ALWAYS_ZERO, FAKE_OVERREPORT,
        FAKE_RETRY_WITH_DATA, FAKE_ERROR_WITH_DATA, FAKE_INVALID_STATUS
    };
    static const vm_block_status_t expected_status[] = {
        VM_BLOCK_STATUS_BACKEND, VM_BLOCK_STATUS_STALLED,
        VM_BLOCK_STATUS_STALLED, VM_BLOCK_STATUS_PROTOCOL,
        VM_BLOCK_STATUS_PROTOCOL, VM_BLOCK_STATUS_PROTOCOL,
        VM_BLOCK_STATUS_PROTOCOL
    };
    fixture_t fixture;
    uint8_t before[16];
    uint8_t guest_source[16];
    size_t i;

    for (i = 0u; i < sizeof(failure_modes) / sizeof(failure_modes[0]); i++) {
        fixture_init(&fixture);
        memset(fixture.ram.bytes + TEST_GUEST, 0xcc, sizeof(before));
        memcpy(before, fixture.ram.bytes + TEST_GUEST, sizeof(before));
        fixture.fake_block.read.mode = failure_modes[i];
        set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                    TEST_TOKEN, (uint32_t)sizeof(before));
        expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                     MD_BRIDGE_ERROR_BLOCK_IO, "hostile read backend");
        CHECK(fixture.bridge.last_error.block_status == expected_status[i],
              "read mode %u status %d expected %d", (unsigned)failure_modes[i],
              (int)fixture.bridge.last_error.block_status,
              (int)expected_status[i]);
        CHECK(memcmp(fixture.ram.bytes + TEST_GUEST, before,
                     sizeof(before)) == 0,
              "failed read mode %u was not atomic",
              (unsigned)failure_modes[i]);
        CHECK(fixture.bridge.stats.successful_reads == 0u &&
              fixture.bridge.stats.bytes_read == 0u,
              "failed read counted as success");
    }

    fixture_init(&fixture);
    memset(fixture.ram.bytes + TEST_GUEST, 0x39, sizeof(before));
    memcpy(before, fixture.ram.bytes + TEST_GUEST, sizeof(before));
    fixture.fake_block.read.mode = FAKE_PARTIAL_THEN_ERROR;
    fixture.fake_block.read.first_chunk = 5u;
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, (uint32_t)sizeof(before));
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_BLOCK_IO, "partial failed read");
    CHECK(fixture.bridge.last_error.block_status == VM_BLOCK_STATUS_BACKEND &&
          fixture.bridge.last_error.transferred == 5u,
          "partial read progress not recorded");
    CHECK(memcmp(fixture.ram.bytes + TEST_GUEST, before, sizeof(before)) == 0,
          "partial failed read leaked scratch into RAM");

    fixture_init(&fixture);
    for (i = 0u; i < sizeof(guest_source); i++)
        fixture.ram.bytes[TEST_GUEST + i] = (uint8_t)(0x90u + i);
    memcpy(guest_source, fixture.ram.bytes + TEST_GUEST,
           sizeof(guest_source));
    fixture.fake_block.write.mode = FAKE_PARTIAL_THEN_ERROR;
    fixture.fake_block.write.first_chunk = 6u;
    set_request(&fixture, MD_BRIDGE_DIRECTION_WRITE, TEST_GUEST,
                TEST_TOKEN, (uint32_t)sizeof(guest_source));
    expect_error(&fixture, MD_BRIDGE_DIRECTION_WRITE,
                 MD_BRIDGE_ERROR_BLOCK_IO, "partial failed write");
    CHECK(fixture.bridge.last_error.block_status == VM_BLOCK_STATUS_BACKEND &&
          fixture.bridge.last_error.transferred == 6u,
          "partial write progress not recorded");
    CHECK(memcmp(fixture.fake_block.bytes, guest_source, 6u) == 0,
          "partial write prefix was not committed as reported");
    CHECK(memcmp(fixture.ram.bytes + TEST_GUEST, guest_source,
                 sizeof(guest_source)) == 0,
          "failed write mutated guest source");
    CHECK(fixture.bridge.stats.successful_writes == 0u &&
          fixture.bridge.stats.bytes_written == 0u,
          "failed write counted as success");
}

static void test_exact_uint32_last_byte(void) {
    fixture_t fixture;
    md_bridge_config_t config;

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.token_base = UINT64_C(0xfffff000);
    config.media_size = UINT64_C(4096);
    fixture.block.size = config.media_size;
    fixture.fake_block.size = (size_t)config.media_size;
    fixture.fake_block.bytes[4095] = 0x7du;
    md_bridge_init(&fixture.bridge, &config);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                UINT64_C(0xffffffff), 1u);
    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_READ) == ARM_SVC_HANDLED &&
          fixture.ram.bytes[TEST_GUEST] == 0x7du,
          "read of exact physical last byte failed");

    fixture.ram.bytes[TEST_GUEST] = 0xb6u;
    set_request(&fixture, MD_BRIDGE_DIRECTION_WRITE, TEST_GUEST,
                UINT64_C(0xffffffff), 1u);
    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_WRITE) == ARM_SVC_HANDLED &&
          fixture.fake_block.bytes[4095] == 0xb6u,
          "write of exact physical last byte failed");
}

static void test_saturating_statistics(void) {
    fixture_t fixture;

    fixture_init(&fixture);
    fixture.bridge.stats.successful_reads = UINT64_MAX - 1u;
    fixture.bridge.stats.bytes_read = UINT64_MAX - 1u;
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 2u);
    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_READ) == ARM_SVC_HANDLED &&
          fixture.bridge.stats.successful_reads == UINT64_MAX &&
          fixture.bridge.stats.bytes_read == UINT64_MAX,
          "read stats did not saturate");
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_READ) == ARM_SVC_HANDLED &&
          fixture.bridge.stats.successful_reads == UINT64_MAX &&
          fixture.bridge.stats.bytes_read == UINT64_MAX,
          "saturated read stats wrapped");

    fixture.bridge.stats.successful_writes = UINT64_MAX;
    fixture.bridge.stats.bytes_written = UINT64_MAX;
    set_request(&fixture, MD_BRIDGE_DIRECTION_WRITE, TEST_GUEST,
                TEST_TOKEN, 1u);
    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_WRITE) == ARM_SVC_HANDLED &&
          fixture.bridge.stats.successful_writes == UINT64_MAX &&
          fixture.bridge.stats.bytes_written == UINT64_MAX,
          "saturated write stats wrapped");

    fixture.bridge.stats.failures = UINT64_MAX;
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 0u);
    expect_error(&fixture, MD_BRIDGE_DIRECTION_READ,
                 MD_BRIDGE_ERROR_LENGTH, "failure saturation");
    CHECK(fixture.bridge.stats.failures == UINT64_MAX,
          "failure counter wrapped");
}

static void test_arm_step_success_and_error(void) {
    fixture_t fixture;
    arm_cpu_t before;
    arm_status_t status;
    uint64_t failures_before;

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 4u);
    put_u32(&fixture.ram, TEST_READ_PC, TEST_READ_SVC);
    arm_bus_set_privileged_svc_handler(&fixture.bus,
                                       md_bridge_handle_svc,
                                       &fixture.bridge);
    fixture.cpu.r[15] = TEST_READ_PC;
    status = arm_step(&fixture.cpu);
    CHECK(status == ARM_OK && fixture.cpu.r[15] == TEST_READ_PC + 2u &&
          fixture.cpu.cycles == 1u && fixture.cpu.r[14] == 0u,
          "successful service did not retire exactly once");
    CHECK(memcmp(fixture.ram.bytes + TEST_GUEST,
                 fixture.fake_block.bytes, 4u) == 0,
          "arm_step service copied wrong bytes");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 4u);
    put_u32(&fixture.ram, TEST_READ_PC, TEST_READ_SVC);
    arm_bus_set_privileged_svc_handler(&fixture.bus,
                                       md_bridge_handle_svc,
                                       &fixture.bridge);
    fixture.fake_block.read.mode = FAKE_ERROR;
    fixture.cpu.r[15] = TEST_READ_PC;
    fixture.cpu.cycles = UINT64_MAX;
    before = fixture.cpu;
    status = arm_step(&fixture.cpu);
    CHECK(status == ARM_HALT, "backend failure did not produce ARM_HALT");
    CHECK(memcmp(&fixture.cpu, &before, sizeof(before)) == 0,
          "ARM_HALT did not restore exact CPU state (including wraparound)");
    CHECK(fixture.bridge.last_error.code == MD_BRIDGE_ERROR_BLOCK_IO &&
          fixture.bridge.last_error.block_status == VM_BLOCK_STATUS_BACKEND,
          "ARM_HALT lost bridge failure detail");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 4u);
    put_u32(&fixture.ram, TEST_READ_PC, TEST_READ_SVC ^ 1u);
    arm_bus_set_privileged_svc_handler(&fixture.bus,
                                       md_bridge_handle_svc,
                                       &fixture.bridge);
    failures_before = fixture.bridge.stats.failures;
    fixture.cpu.r[15] = TEST_READ_PC;
    status = arm_step(&fixture.cpu);
    CHECK(status == ARM_OK && fixture.cpu.r[15] == ARM_VEC_SWI &&
          (fixture.cpu.cpsr & ARM_CPSR_MODE_MASK) == ARM_MODE_SVC,
          "unrecognized immediate did not take ordinary SVC exception");
    CHECK(fixture.bridge.stats.failures == failures_before,
          "ordinary SVC was counted as bridge failure");

    fixture_init(&fixture);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 4u);
    put_u32(&fixture.ram, TEST_READ_PC, TEST_READ_SVC);
    arm_bus_set_privileged_svc_handler(&fixture.bus,
                                       md_bridge_handle_svc,
                                       &fixture.bridge);
    fixture.cpu.cpsr = ARM_MODE_USR | ARM_CPSR_T;
    fixture.cpu.r[15] = TEST_READ_PC;
    status = arm_step(&fixture.cpu);
    CHECK(status == ARM_OK && fixture.cpu.r[15] == ARM_VEC_SWI &&
          fixture.bridge.stats.successful_reads == 0u,
          "user SVC was consumed by host bridge");
}

static void test_exact_patched_thumb_pair(void) {
    fixture_t fixture;
    md_bridge_config_t config;
    arm_cpu_t before;
    arm_cpu_t normalized;
    arm_status_t status;

    fixture_init(&fixture);
    config = fixture.bridge.config;
    config.read_site.encoding = UINT32_C(0xdfe1);
    md_bridge_init(&fixture.bridge, &config);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 4u);

    /* Exact little-endian patch bytes: E1 DF (SVC #e1), C0 46 (MOV r8,r8). */
    put_u32(&fixture.ram, TEST_READ_PC, UINT32_C(0x46c0dfe1));
    arm_bus_set_privileged_svc_handler(&fixture.bus,
                                       md_bridge_handle_svc,
                                       &fixture.bridge);
    fixture.cpu.r[8] = UINT32_C(0x89abcdef);
    fixture.cpu.cpsr |= ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q;
    fixture.cpu.r[15] = TEST_READ_PC;
    before = fixture.cpu;

    status = arm_step(&fixture.cpu);
    normalized = fixture.cpu;
    normalized.r[15] = before.r[15];
    normalized.cycles = before.cycles;
    CHECK(status == ARM_OK && fixture.cpu.r[15] == TEST_READ_PC + 2u &&
          fixture.cpu.cycles == before.cycles + 1u,
          "exact SVC halfword did not retire to its padding instruction");
    CHECK(memcmp(&normalized, &before, sizeof(before)) == 0,
          "exact SVC halfword changed non-PC architectural CPU state");
    CHECK(memcmp(fixture.ram.bytes + TEST_GUEST,
                 fixture.fake_block.bytes, 4u) == 0,
          "exact SVC halfword copied wrong block bytes");

    status = arm_step(&fixture.cpu);
    normalized = fixture.cpu;
    normalized.r[15] = before.r[15];
    normalized.cycles = before.cycles;
    CHECK(status == ARM_OK && fixture.cpu.r[15] == TEST_READ_PC + 4u &&
          fixture.cpu.cycles == before.cycles + 2u,
          "MOV r8,r8 padding did not retire to the original BL fallthrough");
    CHECK(memcmp(&normalized, &before, sizeof(before)) == 0 &&
          fixture.cpu.r[8] == UINT32_C(0x89abcdef) &&
          (fixture.cpu.cpsr & (ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q)) ==
              (ARM_CPSR_N | ARM_CPSR_C | ARM_CPSR_Q),
          "MOV r8,r8 padding changed registers, flags, or other CPU state");
}

static void test_error_strings_and_nulls(void) {
    fixture_t fixture;

    fixture_init(&fixture);
    CHECK(md_bridge_handle_svc(NULL, &fixture.cpu,
                               TEST_READ_PC, TEST_READ_SVC) == ARM_SVC_ERROR,
          "null bridge did not fail closed");
    CHECK(md_bridge_handle_svc(&fixture.bridge, NULL,
                               TEST_READ_PC, TEST_READ_SVC) == ARM_SVC_ERROR &&
          fixture.bridge.last_error.code == MD_BRIDGE_ERROR_NULL_CPU,
          "null CPU did not record stable error");
    CHECK(strcmp(md_bridge_error_string(MD_BRIDGE_ERROR_BLOCK_IO),
                 "block transfer failed") == 0,
          "known error string changed");
    CHECK(strcmp(md_bridge_error_string((md_bridge_error_code_t)999),
                 "unknown bridge error") == 0,
          "unknown error string wrong");

    fixture_init(&fixture);
    fixture.bridge.stats.failures = 17u;
    md_bridge_init(&fixture.bridge, &fixture.bridge.config);
    set_request(&fixture, MD_BRIDGE_DIRECTION_READ, TEST_GUEST,
                TEST_TOKEN, 1u);
    CHECK(invoke(&fixture, MD_BRIDGE_DIRECTION_READ) == ARM_SVC_HANDLED &&
          fixture.bridge.stats.failures == 0u,
          "in-place config reinitialization lost config or retained stats");
    md_bridge_init(NULL, NULL);
}

int main(void) {
    printf("iOS3-VM md bridge tests\n");
    test_success_and_cpu_immutability();
    test_exact_gate_is_unhandled();
    test_configuration_preflight();
    test_mode_and_configuration_fail_closed();
    test_stack_validation();
    test_mmu_stack_and_direct_bulk_ram();
    test_real_ram_base_translation_and_bulk_offsets();
    test_argument_and_range_validation();
    test_partial_retry_and_cancellation();
    test_backend_failures_and_atomicity();
    test_exact_uint32_last_byte();
    test_saturating_statistics();
    test_arm_step_success_and_error();
    test_exact_patched_thumb_pair();
    test_error_strings_and_nulls();
    printf("\n%u passed, %u failed\n", g_passes, g_failures);
    return g_failures == 0u ? 0 : 1;
}

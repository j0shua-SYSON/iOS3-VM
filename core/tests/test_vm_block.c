/*
 * iOS3-VM -- bounded writable block backend tests.
 *
 * These tests use only in-memory callbacks and deliberately exercise hostile
 * partial, retry, cancellation, overflow, and protocol outcomes.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "vm_block.h"

#include <stdio.h>
#include <string.h>

static int g_pass;
static int g_fail;

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

typedef struct {
    unsigned calls;
    size_t max_chunk;
    size_t first_chunk;
    unsigned retries_left;
    unsigned zero_ok_left;
    unsigned error_call;
    bool always_retry;
    bool always_zero;
    bool alternating_no_progress;
    bool overreport;
    unsigned overreport_call;
    bool retry_with_data;
    bool error_with_data;
    bool invalid_status;
} fake_behavior_t;

typedef struct {
    uint8_t data[128];
    size_t data_size;
    fake_behavior_t read;
    fake_behavior_t write;
    unsigned flush_calls;
    unsigned flush_retries_left;
    bool flush_always_retry;
    bool flush_error;
    bool flush_invalid;
} fake_backend_t;

static bool fake_behavior_before_transfer(fake_behavior_t *behavior,
                                          size_t requested,
                                          size_t *actual,
                                          vm_block_io_status_t *status) {
    behavior->calls++;
    *actual = 0;

    if (behavior->invalid_status) {
        *status = (vm_block_io_status_t)99;
        return true;
    }
    if (behavior->overreport ||
        (behavior->overreport_call &&
         behavior->calls == behavior->overreport_call)) {
        if (requested == SIZE_MAX) {
            *actual = requested ? 1u : 0u;
            *status = VM_BLOCK_IO_RETRY;
        } else {
            *actual = requested + 1u;
            *status = VM_BLOCK_IO_OK;
        }
        return true;
    }
    if (behavior->retry_with_data) {
        *actual = requested ? 1u : 0u;
        *status = VM_BLOCK_IO_RETRY;
        return true;
    }
    if (behavior->error_with_data) {
        *actual = requested ? 1u : 0u;
        *status = VM_BLOCK_IO_ERROR;
        return true;
    }
    if (behavior->alternating_no_progress) {
        *status = (behavior->calls & 1u) ? VM_BLOCK_IO_RETRY : VM_BLOCK_IO_OK;
        return true;
    }
    if (behavior->always_retry || behavior->retries_left) {
        if (behavior->retries_left)
            behavior->retries_left--;
        *status = VM_BLOCK_IO_RETRY;
        return true;
    }
    if (behavior->always_zero || behavior->zero_ok_left) {
        if (behavior->zero_ok_left)
            behavior->zero_ok_left--;
        *status = VM_BLOCK_IO_OK;
        return true;
    }
    if (behavior->error_call && behavior->calls == behavior->error_call) {
        *status = VM_BLOCK_IO_ERROR;
        return true;
    }
    return false;
}

static size_t fake_transfer_count(const fake_behavior_t *behavior,
                                  size_t available,
                                  size_t requested) {
    size_t count = requested < available ? requested : available;

    if (behavior->calls == 1u && behavior->first_chunk &&
        count > behavior->first_chunk)
        count = behavior->first_chunk;
    if (behavior->max_chunk && count > behavior->max_chunk)
        count = behavior->max_chunk;
    return count;
}

static vm_block_io_status_t fake_read(void *opaque, uint64_t offset,
                                      void *destination, size_t requested,
                                      size_t *actual) {
    fake_backend_t *fake = (fake_backend_t *)opaque;
    vm_block_io_status_t early_status = VM_BLOCK_IO_OK;
    size_t available;
    size_t count;

    if (fake_behavior_before_transfer(&fake->read, requested, actual,
                                      &early_status))
        return early_status;
    if (offset >= fake->data_size)
        return VM_BLOCK_IO_OK;

    available = fake->data_size - (size_t)offset;
    count = fake_transfer_count(&fake->read, available, requested);
    memcpy(destination, fake->data + (size_t)offset, count);
    *actual = count;
    return VM_BLOCK_IO_OK;
}

static vm_block_io_status_t fake_write(void *opaque, uint64_t offset,
                                       const void *source, size_t requested,
                                       size_t *actual) {
    fake_backend_t *fake = (fake_backend_t *)opaque;
    vm_block_io_status_t early_status = VM_BLOCK_IO_OK;
    size_t available;
    size_t count;

    if (fake_behavior_before_transfer(&fake->write, requested, actual,
                                      &early_status))
        return early_status;
    if (offset >= fake->data_size)
        return VM_BLOCK_IO_OK;

    available = fake->data_size - (size_t)offset;
    count = fake_transfer_count(&fake->write, available, requested);
    memcpy(fake->data + (size_t)offset, source, count);
    *actual = count;
    return VM_BLOCK_IO_OK;
}

static vm_block_io_status_t fake_flush(void *opaque) {
    fake_backend_t *fake = (fake_backend_t *)opaque;

    fake->flush_calls++;
    if (fake->flush_invalid)
        return (vm_block_io_status_t)99;
    if (fake->flush_error)
        return VM_BLOCK_IO_ERROR;
    if (fake->flush_always_retry || fake->flush_retries_left) {
        if (fake->flush_retries_left)
            fake->flush_retries_left--;
        return VM_BLOCK_IO_RETRY;
    }
    return VM_BLOCK_IO_OK;
}

static vm_block_t make_block(fake_backend_t *fake, uint64_t logical_size) {
    vm_block_t block;

    block.context = fake;
    block.size = logical_size;
    block.identity = UINT64_C(0x1122334455667788);
    block.generation = UINT64_C(0x8877665544332211);
    block.read_at = fake_read;
    block.write_at = fake_write;
    block.flush = fake_flush;
    return block;
}

static void test_ranges_and_zero_length_operations(void) {
    vm_block_t block = {0};
    uint8_t byte = 0;
    size_t completed = 99;

    block.size = 12;
    CHECK(vm_block_check_range(&block, 0, 12) == VM_BLOCK_STATUS_OK,
          "whole-block range failed");
    CHECK(vm_block_check_range(&block, 12, 0) == VM_BLOCK_STATUS_OK,
          "empty terminal range failed");
    CHECK(vm_block_check_range(&block, 13, 0) == VM_BLOCK_STATUS_BOUNDS,
          "empty range beyond the end was accepted");
    CHECK(vm_block_check_range(&block, 11, 2) == VM_BLOCK_STATUS_BOUNDS,
          "representable out-of-bounds range was accepted");
    CHECK(vm_block_check_range(NULL, 0, 0) == VM_BLOCK_STATUS_INVALID_ARGUMENT,
          "NULL range descriptor was accepted");

    CHECK(vm_block_read_exact(&block, 12, NULL, 0, NULL, NULL,
                              &completed) == VM_BLOCK_STATUS_OK,
          "empty terminal read required a callback or destination");
    CHECK(completed == 0, "empty read reported %zu bytes", completed);
    CHECK(vm_block_write_exact(&block, 12, NULL, 0, NULL, NULL,
                               &completed) == VM_BLOCK_STATUS_OK,
          "empty terminal write required a callback or source");
    CHECK(completed == 0, "empty write reported %zu bytes", completed);
    CHECK(vm_block_read_exact(&block, 13, NULL, 0, NULL, NULL,
                              NULL) == VM_BLOCK_STATUS_BOUNDS,
          "out-of-bounds empty read was accepted");
    CHECK(vm_block_write_exact(&block, 11, &byte, 2, NULL, NULL,
                               NULL) == VM_BLOCK_STATUS_BOUNDS,
          "out-of-bounds write was accepted");

    block.size = UINT64_MAX;
    CHECK(vm_block_check_range(&block, UINT64_MAX - 3u, 3) == VM_BLOCK_STATUS_OK,
          "valid terminal range near UINT64_MAX failed");
    CHECK(vm_block_check_range(&block, UINT64_MAX - 1u, 1) == VM_BLOCK_STATUS_OK,
          "last representable byte range failed");
    CHECK(vm_block_check_range(&block, UINT64_MAX, 0) == VM_BLOCK_STATUS_OK,
          "empty UINT64_MAX terminal range failed");
    CHECK(vm_block_check_range(&block, UINT64_MAX - 3u, 4) ==
              VM_BLOCK_STATUS_OVERFLOW,
          "overflowing end was not distinguished from bounds");
    CHECK(vm_block_check_range(&block, UINT64_MAX, 1) ==
              VM_BLOCK_STATUS_OVERFLOW,
          "UINT64_MAX + 1 overflow was accepted");
}

static void test_invalid_arguments(void) {
    vm_block_t block = {0};
    uint8_t byte = 0;
    size_t completed = 99;
    vm_block_info_t info;

    block.size = 1;
    CHECK(vm_block_read_exact(NULL, 0, &byte, 1, NULL, NULL,
                              &completed) == VM_BLOCK_STATUS_INVALID_ARGUMENT,
          "NULL read block was accepted");
    CHECK(completed == 0, "NULL read block left progress at %zu", completed);
    CHECK(vm_block_write_exact(NULL, 0, &byte, 1, NULL, NULL,
                               &completed) == VM_BLOCK_STATUS_INVALID_ARGUMENT,
          "NULL write block was accepted");
    CHECK(completed == 0, "NULL write block left progress at %zu", completed);
    CHECK(vm_block_read_exact(&block, 0, NULL, 1, NULL, NULL,
                              NULL) == VM_BLOCK_STATUS_INVALID_ARGUMENT,
          "NULL read destination was accepted");
    CHECK(vm_block_read_exact(&block, 0, &byte, 1, NULL, NULL,
                              NULL) == VM_BLOCK_STATUS_INVALID_ARGUMENT,
          "missing read callback was accepted");
    CHECK(vm_block_write_exact(&block, 0, NULL, 1, NULL, NULL,
                               NULL) == VM_BLOCK_STATUS_INVALID_ARGUMENT,
          "NULL write source was accepted");
    CHECK(vm_block_write_exact(&block, 0, &byte, 1, NULL, NULL,
                               NULL) == VM_BLOCK_STATUS_INVALID_ARGUMENT,
          "missing write callback was accepted");
    CHECK(vm_block_flush(NULL, NULL, NULL) == VM_BLOCK_STATUS_INVALID_ARGUMENT,
          "NULL flush block was accepted");
    CHECK(vm_block_flush(&block, NULL, NULL) == VM_BLOCK_STATUS_INVALID_ARGUMENT,
          "missing flush callback was accepted");
    CHECK(vm_block_get_info(NULL, &info) == VM_BLOCK_STATUS_INVALID_ARGUMENT,
          "NULL info block was accepted");
    CHECK(vm_block_get_info(&block, NULL) == VM_BLOCK_STATUS_INVALID_ARGUMENT,
          "NULL info output was accepted");
}

static void test_partial_reads_and_writes(void) {
    fake_backend_t fake = {0};
    vm_block_t block;
    uint8_t expected[32];
    uint8_t output[32];
    uint8_t input[29];
    size_t completed = 0;

    fake.data_size = sizeof(fake.data);
    for (size_t i = 0; i < fake.data_size; i++)
        fake.data[i] = (uint8_t)(i * 7u + 3u);
    memcpy(expected, fake.data + 7, sizeof(expected));
    fake.read.max_chunk = 3;
    block = make_block(&fake, fake.data_size);

    CHECK(vm_block_read_exact(&block, 7, output, sizeof(output), NULL, NULL,
                              &completed) == VM_BLOCK_STATUS_OK,
          "partial read did not complete");
    CHECK(completed == sizeof(output), "partial read completed %zu", completed);
    CHECK(fake.read.calls == 11u, "partial read used %u callbacks", fake.read.calls);
    CHECK(memcmp(output, expected, sizeof(output)) == 0,
          "partial read changed data");

    for (size_t i = 0; i < sizeof(input); i++)
        input[i] = (uint8_t)(0xf0u - i);
    memset(&fake.write, 0, sizeof(fake.write));
    fake.write.max_chunk = 4;
    CHECK(vm_block_write_exact(&block, 19, input, sizeof(input), NULL, NULL,
                               &completed) == VM_BLOCK_STATUS_OK,
          "partial write did not complete");
    CHECK(completed == sizeof(input), "partial write completed %zu", completed);
    CHECK(fake.write.calls == 8u, "partial write used %u callbacks", fake.write.calls);
    CHECK(memcmp(fake.data + 19, input, sizeof(input)) == 0,
          "partial write changed payload");
    CHECK(fake.data[18] == (uint8_t)(18u * 7u + 3u),
          "partial write damaged preceding byte");
    CHECK(fake.data[48] == (uint8_t)(48u * 7u + 3u),
          "partial write damaged following byte");
}

typedef struct {
    unsigned calls;
    unsigned retries_at_offset;
} reset_progress_t;

static vm_block_io_status_t progress_reset_read(void *opaque, uint64_t offset,
                                                void *destination,
                                                size_t requested,
                                                size_t *actual) {
    reset_progress_t *state = (reset_progress_t *)opaque;

    state->calls++;
    *actual = 0;
    if (state->retries_at_offset < VM_BLOCK_NO_PROGRESS_LIMIT - 1u) {
        state->retries_at_offset++;
        return VM_BLOCK_IO_RETRY;
    }

    state->retries_at_offset = 0;
    if (requested) {
        *(uint8_t *)destination = (uint8_t)offset;
        *actual = 1;
    }
    return VM_BLOCK_IO_OK;
}

static void test_retry_and_no_progress_policy(void) {
    fake_backend_t fake = {0};
    vm_block_t block;
    uint8_t output[4];
    uint8_t input[4] = {9, 8, 7, 6};
    size_t completed = 99;
    reset_progress_t reset = {0};
    vm_block_t reset_block = {0};

    fake.data_size = sizeof(fake.data);
    fake.data[0] = 1;
    fake.data[1] = 2;
    fake.data[2] = 3;
    fake.data[3] = 4;
    fake.read.retries_left = 3;
    block = make_block(&fake, fake.data_size);
    CHECK(vm_block_read_exact(&block, 0, output, sizeof(output), NULL, NULL,
                              &completed) == VM_BLOCK_STATUS_OK,
          "transient read retries did not recover");
    CHECK(fake.read.calls == 4u, "three read retries used %u calls", fake.read.calls);
    CHECK(completed == sizeof(output), "retried read completed %zu", completed);

    fake.write.zero_ok_left = 2;
    CHECK(vm_block_write_exact(&block, 8, input, sizeof(input), NULL, NULL,
                               &completed) == VM_BLOCK_STATUS_OK,
          "transient zero-byte writes did not recover");
    CHECK(fake.write.calls == 3u, "two zero writes used %u calls", fake.write.calls);
    CHECK(memcmp(fake.data + 8, input, sizeof(input)) == 0,
          "retried write changed payload");

    memset(&fake.read, 0, sizeof(fake.read));
    fake.read.always_retry = true;
    CHECK(vm_block_read_exact(&block, 0, output, 1, NULL, NULL,
                              &completed) == VM_BLOCK_STATUS_STALLED,
          "permanent RETRY spun or returned the wrong status");
    CHECK(fake.read.calls == VM_BLOCK_NO_PROGRESS_LIMIT,
          "RETRY cap used %u callbacks", fake.read.calls);
    CHECK(completed == 0, "stalled read completed %zu", completed);

    memset(&fake.write, 0, sizeof(fake.write));
    fake.write.always_zero = true;
    CHECK(vm_block_write_exact(&block, 0, input, 1, NULL, NULL,
                               &completed) == VM_BLOCK_STATUS_STALLED,
          "permanent zero-byte write spun or returned the wrong status");
    CHECK(fake.write.calls == VM_BLOCK_NO_PROGRESS_LIMIT,
          "zero-write cap used %u callbacks", fake.write.calls);
    CHECK(completed == 0, "stalled write completed %zu", completed);

    memset(&fake.read, 0, sizeof(fake.read));
    fake.read.always_zero = true;
    CHECK(vm_block_read_exact(&block, 0, output, 1, NULL, NULL,
                              &completed) == VM_BLOCK_STATUS_STALLED,
          "permanent zero-byte read spun or returned the wrong status");
    CHECK(fake.read.calls == VM_BLOCK_NO_PROGRESS_LIMIT,
          "zero-read cap used %u callbacks", fake.read.calls);

    memset(&fake.write, 0, sizeof(fake.write));
    fake.write.always_retry = true;
    CHECK(vm_block_write_exact(&block, 0, input, 1, NULL, NULL,
                               &completed) == VM_BLOCK_STATUS_STALLED,
          "permanent write RETRY spun or returned the wrong status");
    CHECK(fake.write.calls == VM_BLOCK_NO_PROGRESS_LIMIT,
          "write-RETRY cap used %u callbacks", fake.write.calls);

    memset(&fake.read, 0, sizeof(fake.read));
    fake.read.alternating_no_progress = true;
    CHECK(vm_block_read_exact(&block, 0, output, 1, NULL, NULL,
                              &completed) == VM_BLOCK_STATUS_STALLED,
          "mixed RETRY/zero callbacks did not share one stall counter");
    CHECK(fake.read.calls == VM_BLOCK_NO_PROGRESS_LIMIT,
          "mixed no-progress cap used %u callbacks", fake.read.calls);
    CHECK(completed == 0, "mixed no-progress read completed %zu", completed);

    reset_block.context = &reset;
    reset_block.size = 2;
    reset_block.read_at = progress_reset_read;
    CHECK(vm_block_read_exact(&reset_block, 0, output, 2, NULL, NULL,
                              &completed) == VM_BLOCK_STATUS_OK,
          "positive progress did not reset the no-progress counter");
    CHECK(reset.calls == 2u * VM_BLOCK_NO_PROGRESS_LIMIT,
          "two bounded retry runs used %u callbacks", reset.calls);
    CHECK(completed == 2, "progress-reset read completed %zu", completed);
    CHECK(output[0] == 0 && output[1] == 1,
          "progress-reset read returned wrong bytes");
}

typedef struct {
    size_t read_requests[2];
    size_t write_requests[2];
    uint64_t read_offsets[2];
    uint64_t write_offsets[2];
    unsigned read_calls;
    unsigned write_calls;
    unsigned zero_requests;
    unsigned oversized_requests;
} capped_backend_t;

static uint8_t g_capped_transfer[VM_BLOCK_MAX_CALLBACK_BYTES + 17u];

static vm_block_io_status_t capped_read(void *opaque, uint64_t offset,
                                        void *destination, size_t requested,
                                        size_t *actual) {
    capped_backend_t *capped = (capped_backend_t *)opaque;

    if (capped->read_calls < 2u) {
        capped->read_requests[capped->read_calls] = requested;
        capped->read_offsets[capped->read_calls] = offset;
    }
    capped->read_calls++;
    if (requested == 0)
        capped->zero_requests++;
    if (requested > VM_BLOCK_MAX_CALLBACK_BYTES)
        capped->oversized_requests++;
    memset(destination, (int)(0x40u + capped->read_calls), requested);
    *actual = requested;
    return VM_BLOCK_IO_OK;
}

static vm_block_io_status_t capped_write(void *opaque, uint64_t offset,
                                         const void *source, size_t requested,
                                         size_t *actual) {
    capped_backend_t *capped = (capped_backend_t *)opaque;
    const uint8_t *bytes = (const uint8_t *)source;

    if (capped->write_calls < 2u) {
        capped->write_requests[capped->write_calls] = requested;
        capped->write_offsets[capped->write_calls] = offset;
    }
    capped->write_calls++;
    if (requested == 0)
        capped->zero_requests++;
    if (requested > VM_BLOCK_MAX_CALLBACK_BYTES)
        capped->oversized_requests++;
    if (requested)
        CHECK(bytes[0] != 0, "capped write received an uninitialized byte");
    *actual = requested;
    return VM_BLOCK_IO_OK;
}

static void test_callback_request_cap(void) {
    const uint64_t base = UINT64_C(1) << 32;
    capped_backend_t capped = {0};
    vm_block_t block = {0};
    size_t completed = 0;

    block.context = &capped;
    block.size = base + sizeof(g_capped_transfer);
    block.read_at = capped_read;
    block.write_at = capped_write;

    CHECK(vm_block_read_exact(&block, base, g_capped_transfer,
                              sizeof(g_capped_transfer), NULL, NULL,
                              &completed) == VM_BLOCK_STATUS_OK,
          "large exact read failed");
    CHECK(completed == sizeof(g_capped_transfer),
          "capped read completed %zu bytes", completed);
    CHECK(capped.read_calls == 2u, "capped read used %u callbacks", capped.read_calls);
    CHECK(capped.read_requests[0] == (size_t)VM_BLOCK_MAX_CALLBACK_BYTES,
          "first read request was %zu", capped.read_requests[0]);
    CHECK(capped.read_requests[1] == 17u,
          "terminal read request was %zu", capped.read_requests[1]);
    CHECK(capped.read_offsets[0] == base &&
          capped.read_offsets[1] == base + VM_BLOCK_MAX_CALLBACK_BYTES,
          "capped read advanced the 64-bit offset incorrectly");
    CHECK(g_capped_transfer[0] == 0x41u &&
          g_capped_transfer[VM_BLOCK_MAX_CALLBACK_BYTES] == 0x42u,
          "capped read wrote across the wrong callback boundaries");

    CHECK(vm_block_write_exact(&block, base, g_capped_transfer,
                               sizeof(g_capped_transfer), NULL, NULL,
                               &completed) == VM_BLOCK_STATUS_OK,
          "large exact write failed");
    CHECK(completed == sizeof(g_capped_transfer),
          "capped write completed %zu bytes", completed);
    CHECK(capped.write_calls == 2u, "capped write used %u callbacks", capped.write_calls);
    CHECK(capped.write_requests[0] == (size_t)VM_BLOCK_MAX_CALLBACK_BYTES,
          "first write request was %zu", capped.write_requests[0]);
    CHECK(capped.write_requests[1] == 17u,
          "terminal write request was %zu", capped.write_requests[1]);
    CHECK(capped.write_offsets[0] == base &&
          capped.write_offsets[1] == base + VM_BLOCK_MAX_CALLBACK_BYTES,
          "capped write advanced the 64-bit offset incorrectly");
    CHECK(capped.zero_requests == 0u,
          "large transfer made %u zero-sized callbacks", capped.zero_requests);
    CHECK(capped.oversized_requests == 0u,
          "large transfer made %u oversized callbacks", capped.oversized_requests);
}

static void test_backend_errors_and_protocol(void) {
    fake_backend_t fake = {0};
    vm_block_t block;
    uint8_t buffer[8];
    uint8_t write_input[8];
    size_t completed = 99;

    fake.data_size = sizeof(fake.data);
    for (size_t i = 0; i < sizeof(buffer); i++) {
        fake.data[i] = (uint8_t)(i + 1u);
        write_input[i] = (uint8_t)(0x80u + i);
    }
    memset(buffer, 0xcc, sizeof(buffer));
    fake.read.max_chunk = 3;
    fake.read.error_call = 2;
    block = make_block(&fake, fake.data_size);
    CHECK(vm_block_read_exact(&block, 0, buffer, sizeof(buffer), NULL, NULL,
                              &completed) == VM_BLOCK_STATUS_BACKEND,
          "read backend failure was not reported");
    CHECK(completed == 3, "failed read completed %zu", completed);
    CHECK(memcmp(buffer, fake.data, 3) == 0 && buffer[3] == 0xccu,
          "read error did not preserve only the prior OK progress");

    fake_backend_t write_error = {0};
    write_error.data_size = sizeof(write_error.data);
    memset(write_error.data, 0x5a, sizeof(write_error.data));
    write_error.write.max_chunk = 2;
    write_error.write.error_call = 2;
    block = make_block(&write_error, write_error.data_size);
    CHECK(vm_block_write_exact(&block, 0, write_input, sizeof(write_input), NULL, NULL,
                               &completed) == VM_BLOCK_STATUS_BACKEND,
          "write backend failure was not reported");
    CHECK(completed == 2, "failed write completed %zu", completed);
    CHECK(memcmp(write_error.data, write_input, 2) == 0 &&
          write_error.data[2] == 0x5au,
          "write error did not preserve only the prior OK progress");

    fake_backend_t after_progress = {0};
    after_progress.data_size = sizeof(after_progress.data);
    for (size_t i = 0; i < sizeof(buffer); i++)
        after_progress.data[i] = (uint8_t)(0x20u + i);
    after_progress.read.max_chunk = 3;
    after_progress.read.overreport_call = 2;
    block = make_block(&after_progress, after_progress.data_size);
    memset(buffer, 0xcc, sizeof(buffer));
    CHECK(vm_block_read_exact(&block, 0, buffer, sizeof(buffer), NULL, NULL,
                              &completed) == VM_BLOCK_STATUS_PROTOCOL,
          "read protocol failure after progress was accepted");
    CHECK(completed == 3, "read protocol failure reported %zu bytes", completed);
    CHECK(memcmp(buffer, after_progress.data, 3) == 0 && buffer[3] == 0xccu,
          "read protocol failure lost the committed-prefix boundary");

    memset(after_progress.data, 0x6b, sizeof(after_progress.data));
    after_progress.write.max_chunk = 2;
    after_progress.write.overreport_call = 2;
    CHECK(vm_block_write_exact(&block, 0, write_input, sizeof(write_input), NULL, NULL,
                               &completed) == VM_BLOCK_STATUS_PROTOCOL,
          "write protocol failure after progress was accepted");
    CHECK(completed == 2, "write protocol failure reported %zu bytes", completed);
    CHECK(memcmp(after_progress.data, write_input, 2) == 0 &&
          after_progress.data[2] == 0x6bu,
          "write protocol failure lost the committed-prefix boundary");

    fake_backend_t protocol = {0};
    protocol.data_size = sizeof(protocol.data);
    block = make_block(&protocol, protocol.data_size);
    protocol.read.overreport = true;
    CHECK(vm_block_read_exact(&block, 0, buffer, sizeof(buffer), NULL, NULL,
                              NULL) == VM_BLOCK_STATUS_PROTOCOL,
          "read over-report was accepted");
    memset(&protocol.read, 0, sizeof(protocol.read));
    protocol.read.retry_with_data = true;
    CHECK(vm_block_read_exact(&block, 0, buffer, sizeof(buffer), NULL, NULL,
                              NULL) == VM_BLOCK_STATUS_PROTOCOL,
          "read RETRY with data was accepted");
    memset(&protocol.read, 0, sizeof(protocol.read));
    protocol.read.error_with_data = true;
    CHECK(vm_block_read_exact(&block, 0, buffer, sizeof(buffer), NULL, NULL,
                              NULL) == VM_BLOCK_STATUS_PROTOCOL,
          "read ERROR with data was accepted");
    memset(&protocol.read, 0, sizeof(protocol.read));
    protocol.read.invalid_status = true;
    CHECK(vm_block_read_exact(&block, 0, buffer, sizeof(buffer), NULL, NULL,
                              NULL) == VM_BLOCK_STATUS_PROTOCOL,
          "unknown read status was accepted");

    protocol.write.overreport = true;
    CHECK(vm_block_write_exact(&block, 0, buffer, sizeof(buffer), NULL, NULL,
                               NULL) == VM_BLOCK_STATUS_PROTOCOL,
          "write over-report was accepted");
    memset(&protocol.write, 0, sizeof(protocol.write));
    protocol.write.retry_with_data = true;
    CHECK(vm_block_write_exact(&block, 0, buffer, sizeof(buffer), NULL, NULL,
                               NULL) == VM_BLOCK_STATUS_PROTOCOL,
          "write RETRY with data was accepted");
    memset(&protocol.write, 0, sizeof(protocol.write));
    protocol.write.error_with_data = true;
    CHECK(vm_block_write_exact(&block, 0, buffer, sizeof(buffer), NULL, NULL,
                               NULL) == VM_BLOCK_STATUS_PROTOCOL,
          "write ERROR with data was accepted");
    memset(&protocol.write, 0, sizeof(protocol.write));
    protocol.write.invalid_status = true;
    CHECK(vm_block_write_exact(&block, 0, buffer, sizeof(buffer), NULL, NULL,
                               NULL) == VM_BLOCK_STATUS_PROTOCOL,
          "unknown write status was accepted");
}

typedef struct {
    const unsigned *calls;
    unsigned cancel_after;
    unsigned polls;
} cancel_state_t;

static bool cancel_after_callbacks(void *opaque) {
    cancel_state_t *state = (cancel_state_t *)opaque;

    state->polls++;
    return *state->calls >= state->cancel_after;
}

static void test_cancellation(void) {
    fake_backend_t fake = {0};
    vm_block_t block;
    uint8_t buffer[8] = {0};
    size_t completed = 99;
    cancel_state_t cancel;

    fake.data_size = sizeof(fake.data);
    block = make_block(&fake, fake.data_size);
    cancel.calls = &fake.read.calls;
    cancel.cancel_after = 0;
    cancel.polls = 0;
    CHECK(vm_block_read_exact(&block, 0, buffer, sizeof(buffer),
                              cancel_after_callbacks, &cancel,
                              &completed) == VM_BLOCK_STATUS_CANCELLED,
          "initial read cancellation was ignored");
    CHECK(fake.read.calls == 0u, "initial cancellation made %u reads", fake.read.calls);
    CHECK(completed == 0, "initial cancellation completed %zu", completed);

    fake.read.max_chunk = 3;
    cancel.cancel_after = 1;
    CHECK(vm_block_read_exact(&block, 0, buffer, sizeof(buffer),
                              cancel_after_callbacks, &cancel,
                              &completed) == VM_BLOCK_STATUS_CANCELLED,
          "read cancellation after progress was ignored");
    CHECK(fake.read.calls == 1u, "progress cancellation made %u reads", fake.read.calls);
    CHECK(completed == 3, "progress cancellation completed %zu", completed);

    fake.write.always_retry = true;
    cancel.calls = &fake.write.calls;
    cancel.cancel_after = 1;
    cancel.polls = 0;
    CHECK(vm_block_write_exact(&block, 0, buffer, sizeof(buffer),
                               cancel_after_callbacks, &cancel,
                               &completed) == VM_BLOCK_STATUS_CANCELLED,
          "write cancellation during retry was ignored");
    CHECK(fake.write.calls == 1u, "retry cancellation made %u writes", fake.write.calls);
    CHECK(completed == 0, "retry cancellation completed %zu", completed);

    cancel.calls = &fake.flush_calls;
    cancel.cancel_after = 0;
    cancel.polls = 0;
    CHECK(vm_block_flush(&block, cancel_after_callbacks, &cancel) ==
              VM_BLOCK_STATUS_CANCELLED,
          "initial flush cancellation was ignored");
    CHECK(fake.flush_calls == 0u,
          "initial flush cancellation made %u callbacks", fake.flush_calls);

    fake.flush_always_retry = true;
    cancel.cancel_after = 1;
    cancel.polls = 0;
    CHECK(vm_block_flush(&block, cancel_after_callbacks, &cancel) ==
              VM_BLOCK_STATUS_CANCELLED,
          "flush cancellation during retry was ignored");
    CHECK(fake.flush_calls == 1u, "flush cancellation made %u calls", fake.flush_calls);
}

static void test_flush(void) {
    fake_backend_t fake = {0};
    vm_block_t block = make_block(&fake, 0);

    fake.flush_retries_left = 3;
    CHECK(vm_block_flush(&block, NULL, NULL) == VM_BLOCK_STATUS_OK,
          "transient flush retries did not recover");
    CHECK(fake.flush_calls == 4u, "three flush retries used %u calls", fake.flush_calls);

    fake_backend_t failure = {0};
    block = make_block(&failure, 0);
    failure.flush_error = true;
    CHECK(vm_block_flush(&block, NULL, NULL) == VM_BLOCK_STATUS_BACKEND,
          "flush failure was not reported as a backend error");
    CHECK(failure.flush_calls == 1u, "failed flush used %u calls", failure.flush_calls);

    fake_backend_t invalid = {0};
    block = make_block(&invalid, 0);
    invalid.flush_invalid = true;
    CHECK(vm_block_flush(&block, NULL, NULL) == VM_BLOCK_STATUS_PROTOCOL,
          "unknown flush status was accepted");

    fake_backend_t stalled = {0};
    block = make_block(&stalled, 0);
    stalled.flush_always_retry = true;
    CHECK(vm_block_flush(&block, NULL, NULL) == VM_BLOCK_STATUS_STALLED,
          "permanent flush RETRY spun or returned the wrong status");
    CHECK(stalled.flush_calls == VM_BLOCK_NO_PROGRESS_LIMIT,
          "flush retry cap used %u calls", stalled.flush_calls);
}

typedef struct {
    uint64_t seen_read_offset;
    uint64_t seen_write_offset;
    unsigned read_calls;
    unsigned write_calls;
    unsigned mismatches;
} large_backend_t;

static vm_block_io_status_t large_read(void *opaque, uint64_t offset,
                                       void *destination, size_t requested,
                                       size_t *actual) {
    large_backend_t *large = (large_backend_t *)opaque;
    uint8_t *bytes = (uint8_t *)destination;

    large->seen_read_offset = offset;
    large->read_calls++;
    for (size_t i = 0; i < requested; i++)
        bytes[i] = (uint8_t)((offset + (uint64_t)i) & 0xffu);
    *actual = requested;
    return VM_BLOCK_IO_OK;
}

static vm_block_io_status_t large_write(void *opaque, uint64_t offset,
                                        const void *source, size_t requested,
                                        size_t *actual) {
    large_backend_t *large = (large_backend_t *)opaque;
    const uint8_t *bytes = (const uint8_t *)source;

    large->seen_write_offset = offset;
    large->write_calls++;
    for (size_t i = 0; i < requested; i++) {
        if (bytes[i] != (uint8_t)((offset + (uint64_t)i) & 0xffu))
            large->mismatches++;
    }
    *actual = requested;
    return VM_BLOCK_IO_OK;
}

static void test_large_offsets_and_metadata(void) {
    const uint64_t base = UINT64_C(1) << 32;
    const uint64_t offset = base + 123u;
    large_backend_t large = {0};
    vm_block_t block = {0};
    vm_block_info_t info = {0};
    uint8_t buffer[8];
    size_t completed = 0;

    block.context = &large;
    block.size = base + 4096u;
    block.identity = UINT64_C(0xfedcba9876543210);
    block.generation = UINT64_C(0x89abcdef01234567);
    block.read_at = large_read;
    block.write_at = large_write;

    CHECK(vm_block_read_exact(&block, offset, buffer, sizeof(buffer), NULL, NULL,
                              &completed) == VM_BLOCK_STATUS_OK,
          "read above 4 GiB failed");
    CHECK(large.seen_read_offset == offset,
          "read offset truncated to 0x%llx",
          (unsigned long long)large.seen_read_offset);
    CHECK(large.read_calls == 1u, "large read used %u callbacks", large.read_calls);
    CHECK(completed == sizeof(buffer), "large read completed %zu", completed);
    for (size_t i = 0; i < sizeof(buffer); i++)
        CHECK(buffer[i] == (uint8_t)((offset + (uint64_t)i) & 0xffu),
              "large read byte %zu was %u", i, (unsigned)buffer[i]);

    CHECK(vm_block_write_exact(&block, offset, buffer, sizeof(buffer), NULL, NULL,
                               &completed) == VM_BLOCK_STATUS_OK,
          "write above 4 GiB failed");
    CHECK(large.seen_write_offset == offset,
          "write offset truncated to 0x%llx",
          (unsigned long long)large.seen_write_offset);
    CHECK(large.write_calls == 1u, "large write used %u callbacks", large.write_calls);
    CHECK(large.mismatches == 0u, "large write had %u byte mismatches", large.mismatches);

    CHECK(vm_block_get_info(&block, &info) == VM_BLOCK_STATUS_OK,
          "metadata query failed");
    CHECK(info.size == block.size, "metadata size was truncated");
    CHECK(info.identity == UINT64_C(0xfedcba9876543210),
          "metadata identity was 0x%llx", (unsigned long long)info.identity);
    CHECK(info.generation == UINT64_C(0x89abcdef01234567),
          "metadata generation was 0x%llx", (unsigned long long)info.generation);

    block.generation = UINT64_C(0x89abcdef01234568);
    CHECK(vm_block_get_info(&block, &info) == VM_BLOCK_STATUS_OK,
          "updated metadata query failed");
    CHECK(info.generation == UINT64_C(0x89abcdef01234568),
          "updated generation was not visible");

    vm_block_t unbound = {0};
    CHECK(vm_block_get_info(&unbound, &info) == VM_BLOCK_STATUS_OK,
          "zero metadata query failed");
    CHECK(info.identity == 0 && info.generation == 0,
          "the block layer invented snapshot-binding metadata");
}

static void test_status_strings(void) {
    CHECK(strcmp(vm_block_strerror(VM_BLOCK_STATUS_OK), "success") == 0,
          "success string changed");
    CHECK(strstr(vm_block_strerror(VM_BLOCK_STATUS_OVERFLOW), "overflow") != NULL,
          "overflow string was not descriptive");
    CHECK(strstr(vm_block_strerror(VM_BLOCK_STATUS_BOUNDS), "outside") != NULL,
          "bounds string was not descriptive");
    CHECK(strstr(vm_block_strerror(VM_BLOCK_STATUS_BACKEND), "backend") != NULL,
          "backend string was not descriptive");
    CHECK(strstr(vm_block_strerror(VM_BLOCK_STATUS_PROTOCOL), "protocol") != NULL,
          "protocol string was not descriptive");
    CHECK(strstr(vm_block_strerror((vm_block_status_t)99), "unknown") != NULL,
          "unknown status string changed");
}

int main(void) {
    test_ranges_and_zero_length_operations();
    test_invalid_arguments();
    test_partial_reads_and_writes();
    test_retry_and_no_progress_policy();
    test_callback_request_cap();
    test_backend_errors_and_protocol();
    test_cancellation();
    test_flush();
    test_large_offsets_and_metadata();
    test_status_strings();

    printf("vm_block: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

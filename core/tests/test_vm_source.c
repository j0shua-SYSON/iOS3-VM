/*
 * iOS3-VM -- random-access byte source boundary tests.
 *
 * These deliberately force every awkward callback outcome.  Real host I/O is
 * not involved, so the test stays tiny and deterministic on every platform.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "vm_source.h"

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
    const uint8_t *data;
    size_t data_size;
    size_t max_chunk;
    size_t first_chunk;
    unsigned calls;
    unsigned retries_left;
    unsigned error_call;
    bool report_too_much;
    bool invalid_status;
    bool retry_with_data;
} fake_source_t;

static vm_source_io_status_t fake_read(void *opaque, uint64_t offset,
                                       void *destination, size_t requested,
                                       size_t *actual) {
    fake_source_t *fake = (fake_source_t *)opaque;
    size_t available;
    size_t count;

    fake->calls++;
    *actual = 0;

    if (fake->invalid_status)
        return (vm_source_io_status_t)99;
    if (fake->report_too_much) {
        *actual = requested + 1u;
        return VM_SOURCE_IO_OK;
    }
    if (fake->retry_with_data) {
        *actual = requested ? 1u : 0u;
        return VM_SOURCE_IO_RETRY;
    }
    if (fake->retries_left) {
        fake->retries_left--;
        return VM_SOURCE_IO_RETRY;
    }
    if (fake->error_call && fake->calls == fake->error_call)
        return VM_SOURCE_IO_ERROR;
    if (offset >= fake->data_size)
        return VM_SOURCE_IO_OK;

    available = fake->data_size - (size_t)offset;
    count = requested < available ? requested : available;
    if (fake->calls == 1u && fake->first_chunk && count > fake->first_chunk)
        count = fake->first_chunk;
    if (fake->max_chunk && count > fake->max_chunk)
        count = fake->max_chunk;

    memcpy(destination, fake->data + (size_t)offset, count);
    *actual = count;
    return VM_SOURCE_IO_OK;
}

static vm_source_t make_source(fake_source_t *fake, uint64_t logical_size) {
    vm_source_t source;
    source.context = fake;
    source.size = logical_size;
    source.read_at = fake_read;
    return source;
}

static void test_short_read_at_every_boundary(void) {
    uint8_t input[32];
    uint8_t output[32];

    for (size_t i = 0; i < sizeof(input); i++)
        input[i] = (uint8_t)(i * 7u + 3u);

    for (size_t split = 1; split < sizeof(input); split++) {
        fake_source_t fake = {0};
        vm_source_t source;
        size_t completed = 0;

        fake.data = input;
        fake.data_size = sizeof(input);
        fake.first_chunk = split;
        source = make_source(&fake, sizeof(input));
        memset(output, 0, sizeof(output));

        CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                                   NULL, NULL, &completed) == VM_SOURCE_STATUS_OK,
              "split %zu did not complete", split);
        CHECK(completed == sizeof(output),
              "split %zu completed %zu bytes", split, completed);
        CHECK(memcmp(input, output, sizeof(input)) == 0,
              "split %zu changed data", split);
        CHECK(fake.calls == 2u, "split %zu used %u callbacks", split, fake.calls);
    }

    {
        fake_source_t fake = {0};
        vm_source_t source;
        fake.data = input;
        fake.data_size = sizeof(input);
        fake.max_chunk = 1;
        source = make_source(&fake, sizeof(input));

        CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                                   NULL, NULL, NULL) == VM_SOURCE_STATUS_OK,
              "one-byte chunks did not complete");
        CHECK(fake.calls == sizeof(input), "one-byte chunks used %u callbacks", fake.calls);
        CHECK(memcmp(input, output, sizeof(input)) == 0,
              "one-byte chunks changed data");
    }
}

static void test_exact_eof_and_truncation(void) {
    static const uint8_t input[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t output[8];
    fake_source_t fake = {0};
    vm_source_t source;
    size_t completed = 99;

    fake.data = input;
    fake.data_size = sizeof(input);
    source = make_source(&fake, sizeof(input));
    CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                               NULL, NULL, &completed) == VM_SOURCE_STATUS_OK,
          "read ending exactly at EOF failed");
    CHECK(completed == sizeof(output), "exact EOF completed %zu", completed);
    CHECK(memcmp(input, output, sizeof(input)) == 0, "exact EOF changed data");

    CHECK(vm_source_read_exact(&source, sizeof(input), NULL, 0,
                               NULL, NULL, &completed) == VM_SOURCE_STATUS_OK,
          "empty range at EOF failed");
    CHECK(completed == 0, "empty range reported %zu bytes", completed);
    CHECK(vm_source_read_exact(&source, sizeof(input), output, 1,
                               NULL, NULL, &completed) == VM_SOURCE_STATUS_RANGE,
          "non-empty range at EOF was accepted");

    memset(&fake, 0, sizeof(fake));
    fake.data = input;
    fake.data_size = 4;
    source = make_source(&fake, sizeof(input));
    CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                               NULL, NULL, &completed) == VM_SOURCE_STATUS_EOF,
          "truncated backing did not report EOF");
    CHECK(completed == 4, "truncated backing completed %zu", completed);
    CHECK(fake.calls == 2u, "truncated backing used %u callbacks", fake.calls);
}

static void test_callback_error_and_retry(void) {
    static const uint8_t input[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t output[8];
    fake_source_t fake = {0};
    vm_source_t source;
    size_t completed = 99;

    fake.data = input;
    fake.data_size = sizeof(input);
    fake.error_call = 1;
    source = make_source(&fake, sizeof(input));
    CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                               NULL, NULL, &completed) == VM_SOURCE_STATUS_IO,
          "first callback error was not reported");
    CHECK(completed == 0, "first callback error completed %zu", completed);

    memset(&fake, 0, sizeof(fake));
    fake.data = input;
    fake.data_size = sizeof(input);
    fake.max_chunk = 4;
    fake.error_call = 2;
    source = make_source(&fake, sizeof(input));
    CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                               NULL, NULL, &completed) == VM_SOURCE_STATUS_IO,
          "error after progress was not reported");
    CHECK(completed == 4, "error after progress completed %zu", completed);

    memset(&fake, 0, sizeof(fake));
    fake.data = input;
    fake.data_size = sizeof(input);
    fake.retries_left = 3;
    source = make_source(&fake, sizeof(input));
    CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                               NULL, NULL, &completed) == VM_SOURCE_STATUS_OK,
          "transient retries did not recover");
    CHECK(fake.calls == 4u, "three retries used %u callbacks", fake.calls);
    CHECK(completed == sizeof(output), "retry path completed %zu", completed);
}

static void test_zero_length_and_null_arguments(void) {
    vm_source_t empty = {0};
    uint8_t byte = 0;
    size_t completed = 99;

    empty.size = 12;
    CHECK(vm_source_read_exact(&empty, 12, NULL, 0, NULL, NULL,
                               &completed) == VM_SOURCE_STATUS_OK,
          "zero-length read required a callback or destination");
    CHECK(completed == 0, "zero-length read completed %zu", completed);
    CHECK(vm_source_read_exact(&empty, 13, NULL, 0, NULL, NULL,
                               &completed) == VM_SOURCE_STATUS_RANGE,
          "zero-length range beyond EOF was accepted");
    CHECK(vm_source_read_exact(NULL, 0, &byte, 1, NULL, NULL,
                               &completed) == VM_SOURCE_STATUS_INVALID_ARGUMENT,
          "NULL source was accepted");
    CHECK(completed == 0, "NULL source did not clear out_read");
    CHECK(vm_source_read_exact(&empty, 0, NULL, 1, NULL, NULL,
                               &completed) == VM_SOURCE_STATUS_INVALID_ARGUMENT,
          "NULL destination was accepted");
    CHECK(vm_source_read_exact(&empty, 0, &byte, 1, NULL, NULL,
                               &completed) == VM_SOURCE_STATUS_INVALID_ARGUMENT,
          "NULL callback was accepted");
    CHECK(vm_source_check_range(NULL, 0, 0) == VM_SOURCE_STATUS_INVALID_ARGUMENT,
          "range checker accepted NULL source");
}

static void test_range_overflow_and_large_offsets(void) {
    vm_source_t source = {0};

    source.size = UINT64_MAX;
    CHECK(vm_source_check_range(&source, UINT64_MAX - 3u, 3) == VM_SOURCE_STATUS_OK,
          "valid range near UINT64_MAX failed");
    CHECK(vm_source_check_range(&source, UINT64_MAX - 3u, 4) == VM_SOURCE_STATUS_RANGE,
          "overflowing end near UINT64_MAX was accepted");
    CHECK(vm_source_check_range(&source, UINT64_MAX - 1u, 2) == VM_SOURCE_STATUS_RANGE,
          "offset+length overflow was accepted");
    CHECK(vm_source_check_range(&source, UINT64_MAX, 0) == VM_SOURCE_STATUS_OK,
          "empty terminal UINT64_MAX range failed");
}

typedef struct {
    uint64_t seen_offset;
    unsigned calls;
} large_source_t;

static vm_source_io_status_t large_read(void *opaque, uint64_t offset,
                                        void *destination, size_t requested,
                                        size_t *actual) {
    large_source_t *large = (large_source_t *)opaque;
    uint8_t *bytes = (uint8_t *)destination;

    large->seen_offset = offset;
    large->calls++;
    for (size_t i = 0; i < requested; i++)
        bytes[i] = (uint8_t)((offset + i) & 0xffu);
    *actual = requested;
    return VM_SOURCE_IO_OK;
}

static void test_larger_than_four_gibibytes(void) {
    const uint64_t base = UINT64_C(1) << 32;
    const uint64_t offset = base + 123u;
    large_source_t large = {0};
    vm_source_t source = {&large, base + 4096u, large_read};
    uint8_t output[8];
    size_t completed = 0;

    CHECK(vm_source_check_range(&source, offset, sizeof(output)) == VM_SOURCE_STATUS_OK,
          "range above 4 GiB was rejected");
    CHECK(vm_source_read_exact(&source, offset, output, sizeof(output),
                               NULL, NULL, &completed) == VM_SOURCE_STATUS_OK,
          "read above 4 GiB failed");
    CHECK(large.calls == 1u, "large-offset read used %u callbacks", large.calls);
    CHECK(large.seen_offset == offset,
          "offset was truncated to 0x%llx", (unsigned long long)large.seen_offset);
    CHECK(completed == sizeof(output), "large-offset read completed %zu", completed);
    for (size_t i = 0; i < sizeof(output); i++)
        CHECK(output[i] == (uint8_t)((offset + i) & 0xffu),
              "large-offset byte %zu was %u", i, output[i]);
}

typedef struct {
    const fake_source_t *fake;
    unsigned cancel_after_calls;
    unsigned polls;
} cancel_state_t;

static bool cancel_after_reads(void *opaque) {
    cancel_state_t *state = (cancel_state_t *)opaque;
    state->polls++;
    return state->fake->calls >= state->cancel_after_calls;
}

static void test_cancellation(void) {
    static const uint8_t input[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    uint8_t output[8];
    fake_source_t fake = {0};
    cancel_state_t cancel;
    vm_source_t source;
    size_t completed = 99;

    fake.data = input;
    fake.data_size = sizeof(input);
    source = make_source(&fake, sizeof(input));
    cancel.fake = &fake;
    cancel.cancel_after_calls = 0;
    cancel.polls = 0;
    CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                               cancel_after_reads, &cancel,
                               &completed) == VM_SOURCE_STATUS_CANCELLED,
          "initial cancellation was ignored");
    CHECK(fake.calls == 0u, "initial cancellation made %u callbacks", fake.calls);
    CHECK(completed == 0, "initial cancellation completed %zu", completed);

    memset(&fake, 0, sizeof(fake));
    fake.data = input;
    fake.data_size = sizeof(input);
    fake.max_chunk = 3;
    source = make_source(&fake, sizeof(input));
    cancel.fake = &fake;
    cancel.cancel_after_calls = 1;
    cancel.polls = 0;
    CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                               cancel_after_reads, &cancel,
                               &completed) == VM_SOURCE_STATUS_CANCELLED,
          "cancellation after progress was ignored");
    CHECK(fake.calls == 1u, "progress cancellation made %u callbacks", fake.calls);
    CHECK(completed == 3, "progress cancellation completed %zu", completed);

    memset(&fake, 0, sizeof(fake));
    fake.data = input;
    fake.data_size = sizeof(input);
    fake.retries_left = 4;
    source = make_source(&fake, sizeof(input));
    cancel.fake = &fake;
    cancel.cancel_after_calls = 1;
    cancel.polls = 0;
    CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                               cancel_after_reads, &cancel,
                               &completed) == VM_SOURCE_STATUS_CANCELLED,
          "cancellation during retry was ignored");
    CHECK(fake.calls == 1u, "retry cancellation made %u callbacks", fake.calls);
    CHECK(completed == 0, "retry cancellation completed %zu", completed);
}

static void test_callback_protocol_violations(void) {
    uint8_t output[4];
    fake_source_t fake = {0};
    vm_source_t source = make_source(&fake, sizeof(output));

    fake.report_too_much = true;
    CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                               NULL, NULL, NULL) == VM_SOURCE_STATUS_PROTOCOL,
          "over-reported byte count was accepted");

    memset(&fake, 0, sizeof(fake));
    source = make_source(&fake, sizeof(output));
    fake.invalid_status = true;
    CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                               NULL, NULL, NULL) == VM_SOURCE_STATUS_PROTOCOL,
          "unknown callback status was accepted");

    memset(&fake, 0, sizeof(fake));
    source = make_source(&fake, sizeof(output));
    fake.retry_with_data = true;
    CHECK(vm_source_read_exact(&source, 0, output, sizeof(output),
                               NULL, NULL, NULL) == VM_SOURCE_STATUS_PROTOCOL,
          "retry with progress was accepted");
}

int main(void) {
    test_short_read_at_every_boundary();
    test_exact_eof_and_truncation();
    test_callback_error_and_retry();
    test_zero_length_and_null_arguments();
    test_range_overflow_and_large_offsets();
    test_larger_than_four_gibibytes();
    test_cancellation();
    test_callback_protocol_violations();

    printf("vm_source: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

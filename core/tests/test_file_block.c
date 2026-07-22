/*
 * iOS3-VM -- descriptor-backed vm_block_t adapter tests.
 *
 * Fixtures use tiny relative paths.  CMake pins the test working directory to
 * its F:-backed build tree, never the host's system temporary directory.
 *
 * The production adapter intentionally exposes no syscall-substitution hook:
 * adding one solely to force fstat/fsync EINTR would enlarge the storage trust
 * boundary.  Ordinary I/O executes every identity/revalidation path here;
 * hosted POSIX builds cover descriptor syscalls and hosted MSVC is the required
 * compile-and-runtime coverage for the native Windows handle-identity branch.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef _WIN32
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#define _POSIX_C_SOURCE 200809L
#endif

#include "file_block.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <share.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

static int g_pass;
static int g_fail;
static unsigned g_path_serial;

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

static unsigned long test_process_id(void) {
#ifdef _WIN32
    return (unsigned long)_getpid();
#else
    return (unsigned long)getpid();
#endif
}

static bool make_path(char *destination, size_t capacity, const char *tag) {
    int length;

    g_path_serial++;
    length = snprintf(destination, capacity, "file_block_%lu_%u_%s.img",
                      test_process_id(), g_path_serial, tag);
    return length > 0 && (size_t)length < capacity;
}

static bool create_file(const char *path, const uint8_t *bytes, size_t size) {
    FILE *stream = fopen(path, "wb");
    bool okay = stream != NULL;

    if (okay && size != 0 && fwrite(bytes, 1, size, stream) != size)
        okay = false;
    if (stream && fclose(stream) != 0)
        okay = false;
    return okay;
}

static bool load_file(const char *path, uint8_t *bytes, size_t size) {
    FILE *stream = fopen(path, "rb");
    bool okay = stream != NULL;

    if (okay && size != 0 && fread(bytes, 1, size, stream) != size)
        okay = false;
    if (stream && okay && fgetc(stream) != EOF)
        okay = false;
    if (stream && fclose(stream) != 0)
        okay = false;
    return okay;
}

#ifndef _WIN32
static bool file_size_is(const char *path, uint64_t expected) {
    struct stat status;

    return stat(path, &status) == 0 && status.st_size >= 0 &&
           (uint64_t)status.st_size == expected;
}
#endif

static void fill_pattern(uint8_t *bytes, size_t size, uint8_t seed) {
    size_t index;

    for (index = 0; index < size; index++)
        bytes[index] = (uint8_t)(seed + (uint8_t)(index * 13u));
}

static void test_owner_lifetime(void) {
    char path[160];
    uint8_t bytes[4] = {1, 2, 3, 4};
    uint8_t replacement = 99;
    uint8_t observed[4];
    file_block_t *adapter;
    file_block_t *already_null = NULL;
    const vm_block_t *block;

    CHECK(file_block_destroy(NULL) == FILE_BLOCK_STATUS_INVALID_ARGUMENT,
          "NULL pointer-to-pointer destroy was accepted");
    CHECK(file_block_destroy(&already_null) == FILE_BLOCK_STATUS_OK &&
          already_null == NULL,
          "destroying a NULL owner was not idempotent");

    adapter = file_block_create();
    CHECK(adapter != NULL, "owner allocation failed");
    if (!adapter)
        return;
    CHECK(!file_block_is_open(adapter) && file_block_get(adapter) == NULL,
          "new owner unexpectedly held a session");
    CHECK(file_block_close(adapter) == FILE_BLOCK_STATUS_NOT_OPEN,
          "new owner close did not report not-open");
    CHECK(file_block_flush(adapter) == FILE_BLOCK_STATUS_NOT_OPEN,
          "new owner flush did not report not-open");

    CHECK(make_path(path, sizeof path, "destroy"),
          "could not form destroy fixture path");
    (void)remove(path);
    CHECK(create_file(path, bytes, sizeof bytes),
          "could not create destroy fixture");
    CHECK(file_block_open(adapter, path, sizeof bytes) == FILE_BLOCK_STATUS_OK,
          "could not open destroy fixture");
    block = file_block_get(adapter);
    CHECK(block != NULL, "open destroy fixture exposed no block");
    if (block) {
        CHECK(vm_block_write_exact(block, 2, &replacement, 1, NULL, NULL,
                                   NULL) == VM_BLOCK_STATUS_OK,
              "write before active destroy failed");
    }
    CHECK(file_block_destroy(&adapter) == FILE_BLOCK_STATUS_OK && adapter == NULL,
          "active destroy did not sync, close, free, and clear owner");
    bytes[2] = replacement;
    CHECK(load_file(path, observed, sizeof observed),
          "could not reload actively destroyed fixture");
    CHECK(memcmp(bytes, observed, sizeof bytes) == 0,
          "active destroy did not durably write data");
    CHECK(remove(path) == 0, "could not remove destroy fixture");
}

static void test_random_io_and_guards(void) {
    char path[160];
    uint8_t initial[64];
    uint8_t expected[64];
    uint8_t observed[64];
    uint8_t replacement[11];
    file_block_t *adapter = file_block_create();
    const vm_block_t *block;
    vm_block_info_t info;
    size_t actual = 99;
    size_t completed = 99;
    size_t index;

    CHECK(adapter != NULL, "random I/O owner allocation failed");
    if (!adapter)
        return;
    CHECK(make_path(path, sizeof path, "random"),
          "could not form random fixture path");
    fill_pattern(initial, sizeof initial, 7);
    fill_pattern(replacement, sizeof replacement, 191);
    memcpy(expected, initial, sizeof expected);
    (void)remove(path);
    CHECK(create_file(path, initial, sizeof initial),
          "could not create random fixture");
    CHECK(file_block_open(adapter, path, sizeof initial) == FILE_BLOCK_STATUS_OK,
          "random fixture did not open");
    CHECK(file_block_is_open(adapter), "open random adapter reports closed");
    block = file_block_get(adapter);
    CHECK(block != NULL, "open random adapter exposed no block");
    if (!block) {
        (void)file_block_destroy(&adapter);
        (void)remove(path);
        return;
    }

    CHECK(vm_block_get_info(block, &info) == VM_BLOCK_STATUS_OK,
          "could not query random block metadata");
    CHECK(info.size == sizeof initial, "random block size was %llu",
          (unsigned long long)info.size);
    CHECK(info.identity == 0 && info.generation == 0,
          "writable adapter became snapshot-bindable");

    memset(observed, 0, sizeof observed);
    CHECK(vm_block_read_exact(block, 5, observed, 17, NULL, NULL, &completed) ==
              VM_BLOCK_STATUS_OK && completed == 17,
          "random exact read failed after %zu bytes", completed);
    CHECK(memcmp(observed, initial + 5, 17) == 0,
          "random exact read returned incorrect data");
    CHECK(vm_block_write_exact(block, 23, replacement, sizeof replacement,
                               NULL, NULL, &completed) == VM_BLOCK_STATUS_OK &&
          completed == sizeof replacement,
          "random exact write failed after %zu bytes", completed);
    memcpy(expected + 23, replacement, sizeof replacement);

    /* Descriptor-level positioned I/O must tolerate every direction change. */
    CHECK(vm_block_read_exact(block, 23, observed, sizeof replacement,
                              NULL, NULL, NULL) == VM_BLOCK_STATUS_OK,
          "read immediately after write failed");
    CHECK(memcmp(observed, replacement, sizeof replacement) == 0,
          "read-after-write returned stale bytes");
    CHECK(vm_block_write_exact(block, 0, replacement, 4, NULL, NULL, NULL) ==
              VM_BLOCK_STATUS_OK,
          "write immediately after read failed");
    memcpy(expected, replacement, 4);
    CHECK(vm_block_read_exact(block, 0, observed, 4, NULL, NULL, NULL) ==
              VM_BLOCK_STATUS_OK && memcmp(observed, expected, 4) == 0,
          "second alternating read failed");

    /* Direct-callback protocol guards must fail without poisoning the file. */
    actual = 99;
    CHECK(block->read_at(block->context, 0, NULL, 1, &actual) ==
              VM_BLOCK_IO_ERROR && actual == 0,
          "nonempty NULL read destination was accepted");
    actual = 99;
    CHECK(block->write_at(block->context, 0, NULL, 1, &actual) ==
              VM_BLOCK_IO_ERROR && actual == 0,
          "nonempty NULL write source was accepted");
    CHECK(block->read_at(block->context, 0, observed, 1, NULL) ==
              VM_BLOCK_IO_ERROR,
          "NULL read progress pointer was accepted");
    CHECK(block->write_at(block->context, 0, observed, 1, NULL) ==
              VM_BLOCK_IO_ERROR,
          "NULL write progress pointer was accepted");
    actual = 99;
    CHECK(block->read_at(block->context, sizeof initial, NULL, 0, &actual) ==
              VM_BLOCK_IO_OK && actual == 0,
          "zero-byte direct read failed");
    actual = 99;
    CHECK(block->write_at(block->context, sizeof initial, NULL, 0, &actual) ==
              VM_BLOCK_IO_OK && actual == 0,
          "zero-byte direct write failed");
    actual = 99;
    CHECK(block->read_at(block->context, sizeof initial, observed, 1, &actual) ==
              VM_BLOCK_IO_ERROR && actual == 0,
          "direct out-of-range read was accepted");
    actual = 99;
    CHECK(block->write_at(block->context, 0, observed,
                          (size_t)VM_BLOCK_MAX_CALLBACK_BYTES + 1u, &actual) ==
              VM_BLOCK_IO_ERROR && actual == 0,
          "oversized direct write was accepted");
    CHECK(vm_block_check_range(block, UINT64_MAX, 2) ==
              VM_BLOCK_STATUS_OVERFLOW,
          "overflowing range was not distinguished");
    CHECK(vm_block_read_exact(block, 0, observed, 4, NULL, NULL, NULL) ==
              VM_BLOCK_STATUS_OK,
          "callback guard poisoned a healthy backend");

    CHECK(file_block_flush(adapter) == FILE_BLOCK_STATUS_OK,
          "adapter durable flush failed: host error %d",
          file_block_last_system_error(adapter));
    CHECK(vm_block_flush(block, NULL, NULL) == VM_BLOCK_STATUS_OK,
          "vm_block durable flush failed");
    CHECK(file_block_close(adapter) == FILE_BLOCK_STATUS_OK,
          "random fixture close failed");
    CHECK(!file_block_is_open(adapter) && file_block_get(adapter) == NULL,
          "close did not hide the retired session");
    CHECK(load_file(path, observed, sizeof observed),
          "could not reload random fixture");
    for (index = 0; index < sizeof observed; index++)
        CHECK(observed[index] == expected[index],
              "durable byte %zu was %u, expected %u", index,
              (unsigned)observed[index], (unsigned)expected[index]);

    CHECK(file_block_destroy(&adapter) == FILE_BLOCK_STATUS_OK && adapter == NULL,
          "random owner destroy failed");
    CHECK(remove(path) == 0, "could not remove random fixture");
}

static void test_stale_session_after_reopen(void) {
    char path[160];
    uint8_t bytes[8];
    uint8_t observed = 0;
    uint8_t replacement = 77;
    file_block_t *adapter = file_block_create();
    const vm_block_t *first;
    const vm_block_t *second;
    vm_block_t stale;
    size_t actual;

    CHECK(adapter != NULL, "stale-session owner allocation failed");
    if (!adapter)
        return;
    CHECK(make_path(path, sizeof path, "stale"),
          "could not form stale fixture path");
    fill_pattern(bytes, sizeof bytes, 21);
    (void)remove(path);
    CHECK(create_file(path, bytes, sizeof bytes),
          "could not create stale fixture");
    CHECK(file_block_open(adapter, path, sizeof bytes) == FILE_BLOCK_STATUS_OK,
          "could not open first stale session");
    first = file_block_get(adapter);
    CHECK(first != NULL, "first session exposed no block");
    if (first)
        stale = *first;
    else {
        (void)file_block_destroy(&adapter);
        (void)remove(path);
        return;
    }
    CHECK(file_block_close(adapter) == FILE_BLOCK_STATUS_OK,
          "first session close failed");
    CHECK(file_block_open(adapter, path, sizeof bytes) == FILE_BLOCK_STATUS_OK,
          "could not open second stale session");
    second = file_block_get(adapter);
    CHECK(second != NULL && second != first,
          "reopen reused the retired per-open session");

    actual = 99;
    CHECK(stale.read_at(stale.context, 0, &observed, 1, &actual) ==
              VM_BLOCK_IO_ERROR && actual == 0,
          "stale session read targeted the reopened file");
    actual = 99;
    CHECK(stale.write_at(stale.context, 0, &replacement, 1, &actual) ==
              VM_BLOCK_IO_ERROR && actual == 0,
          "stale session write targeted the reopened file");
    CHECK(vm_block_flush(&stale, NULL, NULL) == VM_BLOCK_STATUS_BACKEND,
          "stale session flush targeted the reopened file");

    if (second) {
        CHECK(vm_block_read_exact(second, 0, &observed, 1, NULL, NULL, NULL) ==
                  VM_BLOCK_STATUS_OK && observed == bytes[0],
              "stale callbacks poisoned the fresh session");
    }
    CHECK(file_block_close(adapter) == FILE_BLOCK_STATUS_OK,
          "second session close failed");
    CHECK(file_block_close(adapter) == FILE_BLOCK_STATUS_NOT_OPEN,
          "second close did not report released ownership");
    CHECK(file_block_destroy(&adapter) == FILE_BLOCK_STATUS_OK && adapter == NULL,
          "stale-session owner destroy failed");
    CHECK(remove(path) == 0, "could not remove stale fixture");
}

#ifndef _WIN32
static bool append_one_byte(const char *path, uint8_t byte) {
    int descriptor;
    ssize_t transferred;
    int saved_error;

    errno = 0;
    descriptor = open(path, O_WRONLY | O_APPEND);
    if (descriptor < 0)
        return false;
    transferred = write(descriptor, &byte, 1);
    saved_error = errno;
    if (close(descriptor) != 0 && transferred == 1)
        transferred = -1;
    errno = saved_error;
    return transferred == 1;
}

static void test_posix_size_and_link_drift(void) {
    char truncate_path[160];
    char extend_path[160];
    char unlink_path[160];
    uint8_t bytes[8];
    uint8_t byte = 0xa5;
    uint8_t observed = 0;
    file_block_t *adapter;
    const vm_block_t *block;
    size_t actual;

    fill_pattern(bytes, sizeof bytes, 31);

    /* A pre-write fstat must catch truncation before pwrite can regrow it. */
    CHECK(make_path(truncate_path, sizeof truncate_path, "truncate"),
          "could not form truncate fixture path");
    (void)remove(truncate_path);
    CHECK(create_file(truncate_path, bytes, sizeof bytes),
          "could not create truncate fixture");
    adapter = file_block_create();
    CHECK(adapter != NULL, "truncate owner allocation failed");
    if (adapter) {
        CHECK(file_block_open(adapter, truncate_path, sizeof bytes) ==
                  FILE_BLOCK_STATUS_OK,
              "could not open truncate fixture");
        block = file_block_get(adapter);
        CHECK(block != NULL, "truncate fixture exposed no block");
        CHECK(truncate(truncate_path, (off_t)3) == 0,
              "could not externally truncate fixture");
        actual = 99;
        if (block) {
            CHECK(block->write_at(block->context, 4, &byte, 1, &actual) ==
                      VM_BLOCK_IO_ERROR && actual == 0,
                  "write regrew a truncated backing");
            CHECK(file_block_last_system_error(adapter) != 0,
                  "truncate drift retained no diagnostic error");
            CHECK(file_size_is(truncate_path, 3),
                  "failed write changed truncated size");
            actual = 99;
            CHECK(block->read_at(block->context, 0, &observed, 1, &actual) ==
                      VM_BLOCK_IO_ERROR && actual == 0,
                  "size drift did not stay sticky");
            CHECK(vm_block_flush(block, NULL, NULL) == VM_BLOCK_STATUS_BACKEND,
                  "size drift vanished at flush");
        }
        CHECK(file_block_close(adapter) == FILE_BLOCK_STATUS_IO,
              "close concealed truncation drift");
        CHECK(file_block_destroy(&adapter) == FILE_BLOCK_STATUS_OK &&
              adapter == NULL,
              "truncate owner destroy failed");
    }
    CHECK(remove(truncate_path) == 0, "could not remove truncate fixture");

    /* Extension is equally invalid even though every guest range is bounded. */
    CHECK(make_path(extend_path, sizeof extend_path, "extend"),
          "could not form extension fixture path");
    (void)remove(extend_path);
    CHECK(create_file(extend_path, bytes, sizeof bytes),
          "could not create extension fixture");
    adapter = file_block_create();
    CHECK(adapter != NULL, "extension owner allocation failed");
    if (adapter) {
        CHECK(file_block_open(adapter, extend_path, sizeof bytes) ==
                  FILE_BLOCK_STATUS_OK,
              "could not open extension fixture");
        block = file_block_get(adapter);
        CHECK(append_one_byte(extend_path, byte),
              "could not externally extend fixture");
        actual = 99;
        if (block) {
            CHECK(block->read_at(block->context, 0, &observed, 1, &actual) ==
                      VM_BLOCK_IO_ERROR && actual == 0,
                  "extended backing remained readable");
            CHECK(file_block_last_system_error(adapter) != 0,
                  "extension drift retained no diagnostic error");
        }
        CHECK(file_block_flush(adapter) == FILE_BLOCK_STATUS_IO,
              "adapter flush concealed extension drift");
        CHECK(file_block_close(adapter) == FILE_BLOCK_STATUS_IO,
              "close concealed extension drift");
        CHECK(file_block_destroy(&adapter) == FILE_BLOCK_STATUS_OK,
              "extension owner destroy failed");
    }
    CHECK(file_size_is(extend_path, sizeof bytes + 1u),
          "extension fixture size was unexpected");
    CHECK(remove(extend_path) == 0, "could not remove extension fixture");

    /* POSIX unlink keeps the fd alive, so st_nlink is the required guard. */
    CHECK(make_path(unlink_path, sizeof unlink_path, "unlink"),
          "could not form unlink fixture path");
    (void)remove(unlink_path);
    CHECK(create_file(unlink_path, bytes, sizeof bytes),
          "could not create unlink fixture");
    adapter = file_block_create();
    CHECK(adapter != NULL, "unlink owner allocation failed");
    if (adapter) {
        CHECK(file_block_open(adapter, unlink_path, sizeof bytes) ==
                  FILE_BLOCK_STATUS_OK,
              "could not open unlink fixture");
        block = file_block_get(adapter);
        CHECK(unlink(unlink_path) == 0, "could not unlink live fixture");
        actual = 99;
        if (block) {
            CHECK(block->read_at(block->context, 0, &observed, 1, &actual) ==
                      VM_BLOCK_IO_ERROR && actual == 0,
                  "unlinked backing remained readable");
        }
        CHECK(file_block_close(adapter) == FILE_BLOCK_STATUS_IO,
              "close concealed zero-link backing");
        CHECK(file_block_destroy(&adapter) == FILE_BLOCK_STATUS_OK,
              "unlink owner destroy failed");
    }
}
#else
static void test_windows_exclusive_sharing(void) {
    char path[160];
    uint8_t bytes[8];
    file_block_t *adapter = file_block_create();
    int competing = -1;

    CHECK(adapter != NULL, "sharing owner allocation failed");
    if (!adapter)
        return;
    CHECK(make_path(path, sizeof path, "sharing"),
          "could not form sharing fixture path");
    fill_pattern(bytes, sizeof bytes, 41);
    (void)remove(path);
    CHECK(create_file(path, bytes, sizeof bytes),
          "could not create sharing fixture");
    CHECK(file_block_open(adapter, path, sizeof bytes) == FILE_BLOCK_STATUS_OK,
          "could not open sharing fixture");

    errno = 0;
#ifdef _MSC_VER
    {
        errno_t open_error = _sopen_s(&competing, path,
                                      _O_RDWR | _O_BINARY | _O_NOINHERIT,
                                      _SH_DENYNO, _S_IREAD | _S_IWRITE);
        CHECK(open_error != 0 && competing < 0,
              "exclusive descriptor allowed a competing Windows open");
    }
#else
    competing = _sopen(path, _O_RDWR | _O_BINARY | _O_NOINHERIT,
                       _SH_DENYNO, _S_IREAD | _S_IWRITE);
    CHECK(competing < 0,
          "exclusive descriptor allowed a competing Windows open");
#endif
    if (competing >= 0)
        (void)_close(competing);

    CHECK(file_block_close(adapter) == FILE_BLOCK_STATUS_OK,
          "sharing fixture close failed");
    CHECK(file_block_destroy(&adapter) == FILE_BLOCK_STATUS_OK,
          "sharing owner destroy failed");
    CHECK(remove(path) == 0, "could not remove sharing fixture");
}
#endif

static void test_open_errors_and_empty_file(void) {
    char path[160];
    char missing[160];
    uint8_t bytes[4] = {3, 1, 4, 1};
    uint8_t byte = 0;
    file_block_t *adapter = file_block_create();
    const vm_block_t *block;

    CHECK(adapter != NULL, "open-error owner allocation failed");
    if (!adapter)
        return;
    CHECK(file_block_open(NULL, "x", 0) ==
              FILE_BLOCK_STATUS_INVALID_ARGUMENT,
          "NULL owner open was accepted");
    CHECK(file_block_open(adapter, NULL, 0) ==
              FILE_BLOCK_STATUS_INVALID_ARGUMENT,
          "NULL path open was accepted");
    CHECK(file_block_open(adapter, "", 0) ==
              FILE_BLOCK_STATUS_INVALID_ARGUMENT,
          "empty path open was accepted");

    CHECK(make_path(missing, sizeof missing, "missing"),
          "could not form missing path");
    (void)remove(missing);
    CHECK(file_block_open(adapter, missing, 0) ==
              FILE_BLOCK_STATUS_OPEN_FAILED,
          "missing file did not report open failure");
    CHECK(file_block_last_system_error(adapter) != 0,
          "open failure retained no exact host error");
    CHECK(!file_block_is_open(adapter), "failed open retained ownership");

    CHECK(make_path(path, sizeof path, "open"),
          "could not form open fixture path");
    (void)remove(path);
    CHECK(create_file(path, bytes, sizeof bytes),
          "could not create open fixture");
    CHECK(file_block_open(adapter, path, sizeof bytes + 1u) ==
              FILE_BLOCK_STATUS_SIZE_MISMATCH,
          "wrong exact size was accepted");
    CHECK(file_block_open(adapter, path, UINT64_MAX) ==
              FILE_BLOCK_STATUS_OFFSET_UNSUPPORTED,
          "unrepresentable descriptor offset was accepted");
    CHECK(file_block_open(adapter, ".", 0) != FILE_BLOCK_STATUS_OK,
          "directory was accepted as a regular file block");
    CHECK(file_block_open(adapter, path, sizeof bytes) == FILE_BLOCK_STATUS_OK,
          "owner was not reusable after failed opens");
    CHECK(file_block_open(adapter, path, sizeof bytes) ==
              FILE_BLOCK_STATUS_ALREADY_OPEN,
          "second open did not preserve close authority");
    CHECK(file_block_close(adapter) == FILE_BLOCK_STATUS_OK,
          "open fixture close failed");
    CHECK(remove(path) == 0, "could not remove open fixture");

    CHECK(make_path(path, sizeof path, "empty"),
          "could not form empty fixture path");
    (void)remove(path);
    CHECK(create_file(path, NULL, 0), "could not create empty fixture");
    CHECK(file_block_open(adapter, path, 0) == FILE_BLOCK_STATUS_OK,
          "zero-length file did not open");
    block = file_block_get(adapter);
    CHECK(block != NULL && block->size == 0,
          "zero-length block metadata was wrong");
    if (block) {
        CHECK(vm_block_read_exact(block, 0, NULL, 0, NULL, NULL, NULL) ==
                  VM_BLOCK_STATUS_OK,
              "zero-length exact read failed");
        CHECK(vm_block_write_exact(block, 0, NULL, 0, NULL, NULL, NULL) ==
                  VM_BLOCK_STATUS_OK,
              "zero-length exact write failed");
        CHECK(vm_block_read_exact(block, 0, &byte, 1, NULL, NULL, NULL) ==
                  VM_BLOCK_STATUS_BOUNDS,
              "nonempty read of empty file was accepted");
    }
    CHECK(file_block_flush(adapter) == FILE_BLOCK_STATUS_OK,
          "empty block flush failed");
    CHECK(file_block_close(adapter) == FILE_BLOCK_STATUS_OK,
          "empty block close failed");
    CHECK(file_block_destroy(&adapter) == FILE_BLOCK_STATUS_OK && adapter == NULL,
          "open-error owner destroy failed");
    CHECK(remove(path) == 0, "could not remove empty fixture");
}

static void test_status_strings(void) {
    CHECK(strcmp(file_block_strerror(FILE_BLOCK_STATUS_OK), "success") == 0,
          "success text changed");
    CHECK(strstr(file_block_strerror(FILE_BLOCK_STATUS_LOCK_FAILED), "lock") != NULL,
          "lock failure text was not descriptive");
    CHECK(strstr(file_block_strerror(FILE_BLOCK_STATUS_SIZE_MISMATCH), "size") != NULL,
          "size mismatch text was not descriptive");
    CHECK(strstr(file_block_strerror(FILE_BLOCK_STATUS_IO), "I/O") != NULL,
          "I/O text was not descriptive");
    CHECK(strstr(file_block_strerror((file_block_status_t)99), "unknown") != NULL,
          "unknown status text changed");
}

int main(void) {
    test_owner_lifetime();
    test_random_io_and_guards();
    test_stale_session_after_reopen();
#ifndef _WIN32
    test_posix_size_and_link_drift();
#else
    test_windows_exclusive_sharing();
#endif
    test_open_errors_and_empty_file();
    test_status_strings();

    printf("file_block: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

/*
 * iOS3-VM -- bounded host-side rootfs work-image provisioning.
 *
 * The source image is opened read-only and is never exposed through a writable
 * handle.  All edits happen under an exclusive, unpublished temporary name in
 * the destination directory.  Publication is no-replace and atomic at the
 * directory-entry level.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef _WIN32
#ifndef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64
#endif
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif

#include "rootfs_work.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define HFS_VH_OFF 1024u
#define HFS_VH_LEN 512u
#define HFS_SIG_HFSPLUS 0x482bu
#define HFS_SIG_HFSX 0x4858u
#define HFS_ATTR_UNMOUNTED (1u << 8)
#define HFS_ATTR_BOOT_INCONSISTENT (1u << 11)
#define HFS_ATTR_JOURNALED (1u << 13)
#define HFS_ATTR_SOFTWARE_LOCK (1u << 15)
#define ROOTFS_TEMP_ATTEMPTS 128u
#define ROOTFS_EINTR_RETRY_LIMIT 64u

static const uint8_t FSTAB_STOCK[] =
    "/dev/disk0s1 / hfs ro 0 1\n"
    "/dev/disk0s2 /private/var hfs rw,nosuid,nodev 0 2\n";

typedef struct host_file {
#ifdef _WIN32
    HANDLE handle;
#else
    int descriptor;
#endif
} host_file_t;

typedef struct file_stamp {
    uint64_t size;
    uint64_t identity_a;
    uint64_t identity_b;
    uint64_t modified_a;
    uint64_t modified_b;
    uint64_t changed_a;
    uint64_t changed_b;
    uint32_t links;
} file_stamp_t;

typedef struct destination_dir {
#ifdef _WIN32
    HANDLE handle;
    char *full_path;
    char *destination_path;
    char *temporary_path;
#else
    int descriptor;
    char *leaf;
    char temporary_leaf[80];
#endif
} destination_dir_t;

typedef struct hfs_volume {
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t next_alloc;
    uint32_t attributes;
    uint64_t alloc_bytes;
    uint32_t alloc_fork_blocks;
    uint32_t ext_start[8];
    uint32_t ext_count[8];
    uint32_t nbits;
} hfs_volume_t;

static uint16_t read_be16(const uint8_t *bytes) {
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t read_be32(const uint8_t *bytes) {
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static uint64_t read_be64(const uint8_t *bytes) {
    return ((uint64_t)read_be32(bytes) << 32) | read_be32(bytes + 4);
}

static void write_be32(uint8_t *bytes, uint32_t value) {
    bytes[0] = (uint8_t)(value >> 24);
    bytes[1] = (uint8_t)(value >> 16);
    bytes[2] = (uint8_t)(value >> 8);
    bytes[3] = (uint8_t)value;
}

static void result_reset(rootfs_work_result_t *result) {
    memset(result, 0, sizeof(*result));
    result->status = ROOTFS_WORK_OK;
    result->stage = ROOTFS_WORK_STAGE_NONE;
    result->fstab_offset = UINT64_MAX;
    result->detail[0] = '\0';
}

static rootfs_work_status_t result_fail(rootfs_work_result_t *result,
                                        rootfs_work_status_t status,
                                        rootfs_work_stage_t stage,
                                        int system_error,
                                        const char *format, ...) {
    va_list arguments;

    result->status = status;
    result->stage = stage;
    result->system_error = system_error;
    va_start(arguments, format);
    (void)vsnprintf(result->detail, sizeof(result->detail), format, arguments);
    va_end(arguments);
    result->detail[sizeof(result->detail) - 1u] = '\0';
    return status;
}

const char *rootfs_work_status_name(rootfs_work_status_t status) {
    switch (status) {
    case ROOTFS_WORK_OK: return "ok";
    case ROOTFS_WORK_INVALID_ARGUMENT: return "invalid-argument";
    case ROOTFS_WORK_NO_MEMORY: return "no-memory";
    case ROOTFS_WORK_PATH_UNSAFE: return "unsafe-path";
    case ROOTFS_WORK_SOURCE_OPEN_FAILED: return "source-open-failed";
    case ROOTFS_WORK_SOURCE_NOT_REGULAR: return "source-not-regular";
    case ROOTFS_WORK_SOURCE_ALIAS: return "source-alias";
    case ROOTFS_WORK_SOURCE_BUSY: return "source-busy";
    case ROOTFS_WORK_DESTINATION_EXISTS: return "destination-exists";
    case ROOTFS_WORK_DESTINATION_OPEN_FAILED: return "destination-open-failed";
    case ROOTFS_WORK_TEMP_CREATE_FAILED: return "temp-create-failed";
    case ROOTFS_WORK_READ_FAILED: return "read-failed";
    case ROOTFS_WORK_WRITE_FAILED: return "write-failed";
    case ROOTFS_WORK_SYNC_FAILED: return "sync-failed";
    case ROOTFS_WORK_SOURCE_CHANGED: return "source-changed";
    case ROOTFS_WORK_SOURCE_IDENTITY_MISMATCH:
        return "source-identity-mismatch";
    case ROOTFS_WORK_HFS_INVALID: return "hfs-invalid";
    case ROOTFS_WORK_FSTAB_NOT_UNIQUE: return "fstab-not-unique";
    case ROOTFS_WORK_FSTAB_LINE_INVALID: return "fstab-line-invalid";
    case ROOTFS_WORK_GROW_INVALID: return "grow-invalid";
    case ROOTFS_WORK_RANGE_ERROR: return "range-error";
    case ROOTFS_WORK_PUBLISH_FAILED: return "publish-failed";
    case ROOTFS_WORK_PUBLISH_DURABILITY_FAILED:
        return "publish-durability-failed";
    }
    return "unknown";
}

const char *rootfs_work_stage_name(rootfs_work_stage_t stage) {
    switch (stage) {
    case ROOTFS_WORK_STAGE_NONE: return "none";
    case ROOTFS_WORK_STAGE_ARGUMENTS: return "arguments";
    case ROOTFS_WORK_STAGE_SOURCE_PATH: return "source-path";
    case ROOTFS_WORK_STAGE_DESTINATION_PATH: return "destination-path";
    case ROOTFS_WORK_STAGE_SOURCE_OPEN: return "source-open";
    case ROOTFS_WORK_STAGE_SOURCE_VALIDATE: return "source-validate";
    case ROOTFS_WORK_STAGE_SOURCE_IDENTITY: return "source-identity";
    case ROOTFS_WORK_STAGE_TEMP_CREATE: return "temp-create";
    case ROOTFS_WORK_STAGE_COPY: return "copy";
    case ROOTFS_WORK_STAGE_COPY_VERIFY: return "copy-verify";
    case ROOTFS_WORK_STAGE_FSTAB_SCAN: return "fstab-scan";
    case ROOTFS_WORK_STAGE_FSTAB_WRITE: return "fstab-write";
    case ROOTFS_WORK_STAGE_GROW_PLAN: return "grow-plan";
    case ROOTFS_WORK_STAGE_GROW_WRITE: return "grow-write";
    case ROOTFS_WORK_STAGE_FINAL_VALIDATE: return "final-validate";
    case ROOTFS_WORK_STAGE_FLUSH: return "flush";
    case ROOTFS_WORK_STAGE_PUBLISH: return "publish";
    case ROOTFS_WORK_STAGE_DIRECTORY_SYNC: return "directory-sync";
    case ROOTFS_WORK_STAGE_CLEANUP: return "cleanup";
    }
    return "unknown";
}

static char *copy_string_n(const char *source, size_t length) {
    char *copy;

    if (length == SIZE_MAX)
        return NULL;
    copy = (char *)malloc(length + 1u);
    if (!copy)
        return NULL;
    memcpy(copy, source, length);
    copy[length] = '\0';
    return copy;
}

static bool stamp_equal(const file_stamp_t *left, const file_stamp_t *right) {
    return left->size == right->size &&
           left->identity_a == right->identity_a &&
           left->identity_b == right->identity_b &&
           left->modified_a == right->modified_a &&
           left->modified_b == right->modified_b &&
           left->changed_a == right->changed_a &&
           left->changed_b == right->changed_b &&
           left->links == right->links;
}

#ifdef _WIN32

static void host_file_init(host_file_t *file) {
    file->handle = INVALID_HANDLE_VALUE;
}

static bool host_file_is_open(const host_file_t *file) {
    return file->handle != INVALID_HANDLE_VALUE;
}

static int windows_error(void) {
    DWORD value = GetLastError();
    return value > (DWORD)INT_MAX ? INT_MAX : (int)value;
}

static bool host_file_close(host_file_t *file, int *system_error) {
    HANDLE handle;

    if (!host_file_is_open(file))
        return true;
    handle = file->handle;
    file->handle = INVALID_HANDLE_VALUE;
    if (CloseHandle(handle))
        return true;
    *system_error = windows_error();
    return false;
}

static bool host_file_read(host_file_t *file, uint64_t offset, void *buffer,
                           size_t length, int *system_error) {
    uint8_t *next = (uint8_t *)buffer;

    while (length != 0u) {
        LARGE_INTEGER position;
        DWORD request = length > (size_t)UINT32_MAX ? UINT32_MAX :
                        (DWORD)length;
        DWORD actual = 0;

        position.QuadPart = (LONGLONG)offset;
        if (offset > (uint64_t)LLONG_MAX ||
            !SetFilePointerEx(file->handle, position, NULL, FILE_BEGIN)) {
            *system_error = windows_error();
            return false;
        }
        if (!ReadFile(file->handle, next, request, &actual, NULL)) {
            *system_error = windows_error();
            return false;
        }
        if (actual == 0u) {
            *system_error = ERROR_HANDLE_EOF;
            return false;
        }
        next += actual;
        offset += actual;
        length -= actual;
    }
    return true;
}

static bool host_file_write(host_file_t *file, uint64_t offset,
                            const void *buffer, size_t length,
                            int *system_error) {
    const uint8_t *next = (const uint8_t *)buffer;

    while (length != 0u) {
        LARGE_INTEGER position;
        DWORD request = length > (size_t)UINT32_MAX ? UINT32_MAX :
                        (DWORD)length;
        DWORD actual = 0;

        position.QuadPart = (LONGLONG)offset;
        if (offset > (uint64_t)LLONG_MAX ||
            !SetFilePointerEx(file->handle, position, NULL, FILE_BEGIN)) {
            *system_error = windows_error();
            return false;
        }
        if (!WriteFile(file->handle, next, request, &actual, NULL)) {
            *system_error = windows_error();
            return false;
        }
        if (actual == 0u) {
            *system_error = ERROR_WRITE_FAULT;
            return false;
        }
        next += actual;
        offset += actual;
        length -= actual;
    }
    return true;
}

static bool host_file_resize(host_file_t *file, uint64_t size,
                             int *system_error) {
    LARGE_INTEGER position;

    if (size > (uint64_t)LLONG_MAX) {
        *system_error = ERROR_ARITHMETIC_OVERFLOW;
        return false;
    }
    position.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx(file->handle, position, NULL, FILE_BEGIN) ||
        !SetEndOfFile(file->handle)) {
        *system_error = windows_error();
        return false;
    }
    return true;
}

static bool host_file_sync(host_file_t *file, int *system_error) {
    if (FlushFileBuffers(file->handle))
        return true;
    *system_error = windows_error();
    return false;
}

static bool host_file_stamp(host_file_t *file, file_stamp_t *stamp,
                            int *system_error) {
    BY_HANDLE_FILE_INFORMATION info;

    if (!GetFileInformationByHandle(file->handle, &info)) {
        *system_error = windows_error();
        return false;
    }
    stamp->size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    stamp->identity_a = info.dwVolumeSerialNumber;
    stamp->identity_b = ((uint64_t)info.nFileIndexHigh << 32) |
                        info.nFileIndexLow;
    stamp->modified_a = info.ftLastWriteTime.dwHighDateTime;
    stamp->modified_b = info.ftLastWriteTime.dwLowDateTime;
    stamp->changed_a = 0;
    stamp->changed_b = 0;
    stamp->links = info.nNumberOfLinks;
    return true;
}

static bool windows_full_path(const char *path, char **full, int *system_error) {
    DWORD needed;
    DWORD written;
    char *buffer;

    needed = GetFullPathNameA(path, 0, NULL, NULL);
    if (needed == 0u) {
        *system_error = windows_error();
        return false;
    }
    buffer = (char *)malloc((size_t)needed + 1u);
    if (!buffer) {
        *system_error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    written = GetFullPathNameA(path, needed + 1u, buffer, NULL);
    if (written == 0u || written > needed) {
        *system_error = windows_error();
        free(buffer);
        return false;
    }
    buffer[written] = '\0';
    *full = buffer;
    return true;
}

static const char *windows_without_device_prefix(const char *path) {
    return strlen(path) >= 4u && path[0] == '\\' && path[1] == '\\' && path[2] == '?' &&
           path[3] == '\\' ? path + 4 : path;
}

static size_t windows_trimmed_length(const char *path) {
    size_t length = strlen(path);

    while (length > 3u &&
           (path[length - 1u] == '\\' || path[length - 1u] == '/'))
        length--;
    return length;
}

static bool windows_paths_equal(const char *left, const char *right) {
    size_t left_length;
    size_t right_length;
    size_t index;

    left = windows_without_device_prefix(left);
    right = windows_without_device_prefix(right);
    left_length = windows_trimmed_length(left);
    right_length = windows_trimmed_length(right);
    if (left_length != right_length)
        return false;
    for (index = 0; index < left_length; index++) {
        unsigned char left_byte = (unsigned char)left[index];
        unsigned char right_byte = (unsigned char)right[index];

        if (left_byte == '/')
            left_byte = '\\';
        if (right_byte == '/')
            right_byte = '\\';
        if (left_byte >= 'a' && left_byte <= 'z')
            left_byte = (unsigned char)(left_byte - ('a' - 'A'));
        if (right_byte >= 'a' && right_byte <= 'z')
            right_byte = (unsigned char)(right_byte - ('a' - 'A'));
        if (left_byte != right_byte)
            return false;
    }
    return true;
}

/* GetFullPathName is lexical.  Pair it with the opened object's canonical
 * handle path so a reparse/rename between inspection and open is a refusal. */
static bool windows_handle_matches_path(HANDLE handle, const char *expected,
                                        int *system_error) {
    DWORD needed;
    DWORD written;
    char *actual;
    bool matches;

    needed = GetFinalPathNameByHandleA(handle, NULL, 0,
                                       FILE_NAME_NORMALIZED |
                                           VOLUME_NAME_DOS);
    if (needed == 0u) {
        *system_error = windows_error();
        return false;
    }
    actual = (char *)malloc((size_t)needed + 1u);
    if (!actual) {
        *system_error = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    written = GetFinalPathNameByHandleA(handle, actual, needed + 1u,
                                        FILE_NAME_NORMALIZED |
                                            VOLUME_NAME_DOS);
    if (written == 0u || written > needed) {
        *system_error = windows_error();
        free(actual);
        return false;
    }
    actual[written] = '\0';
    matches = windows_paths_equal(actual, expected);
    free(actual);
    if (!matches)
        *system_error = ERROR_INVALID_NAME;
    return matches;
}

static bool windows_path_shape_safe(const char *full) {
    size_t index;

    if (!full || strlen(full) < 3u)
        return false;
    /* Network/reparse semantics vary; the work image is intentionally local. */
    if (full[0] == '\\' || full[1] != ':' ||
        (full[2] != '\\' && full[2] != '/'))
        return false;
    for (index = 2u; full[index] != '\0'; index++) {
        if (full[index] == ':')
            return false; /* alternate data stream or malformed drive path */
    }
    return true;
}

/* Validate every existing component and reject all reparse traversal. */
static bool windows_validate_chain(char *full, bool include_final,
                                   int *system_error, bool *unsafe) {
    size_t length = strlen(full);
    size_t index;

    *unsafe = false;
    if (!windows_path_shape_safe(full)) {
        *unsafe = true;
        return false;
    }
    for (index = 3u; index <= length; index++) {
        char saved;
        DWORD attributes;
        bool boundary = full[index] == '\0' || full[index] == '\\' ||
                        full[index] == '/';

        if (!boundary || (!include_final && full[index] == '\0'))
            continue;
        saved = full[index];
        full[index] = '\0';
        attributes = GetFileAttributesA(full);
        full[index] = saved;
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            *system_error = windows_error();
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ||
            (saved != '\0' &&
             (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u)) {
            *unsafe = true;
            return false;
        }
    }
    return true;
}

static bool windows_split_destination(const char *path,
                                      destination_dir_t *destination,
                                      rootfs_work_result_t *result) {
    char *separator;
    DWORD attributes;
    int error = 0;
    bool unsafe = false;

    if (!windows_full_path(path, &destination->destination_path, &error)) {
        result_fail(result, error == ERROR_NOT_ENOUGH_MEMORY ?
                        ROOTFS_WORK_NO_MEMORY :
                        ROOTFS_WORK_DESTINATION_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, error,
                    "cannot resolve destination path");
        return false;
    }
    if (!windows_path_shape_safe(destination->destination_path)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                    "destination must be a local drive path without streams");
        return false;
    }
    separator = strrchr(destination->destination_path, '\\');
    {
        char *forward = strrchr(destination->destination_path, '/');
        if (forward && (!separator || forward > separator))
            separator = forward;
    }
    if (!separator || separator[1] == '\0' ||
        strcmp(separator + 1, ".") == 0 ||
        strcmp(separator + 1, "..") == 0) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                    "destination has no safe file name");
        return false;
    }
    destination->full_path = copy_string_n(destination->destination_path,
                                            (size_t)(separator -
                                                     destination->destination_path));
    if (!destination->full_path) {
        result_fail(result, ROOTFS_WORK_NO_MEMORY,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                    "cannot allocate destination directory path");
        return false;
    }
    if (strlen(destination->full_path) == 2u) {
        char *root = (char *)realloc(destination->full_path, 4u);
        if (!root) {
            result_fail(result, ROOTFS_WORK_NO_MEMORY,
                        ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                        "cannot allocate destination root path");
            return false;
        }
        root[2] = '\\';
        root[3] = '\0';
        destination->full_path = root;
    }
    if (!windows_validate_chain(destination->full_path, true, &error,
                                &unsafe)) {
        result_fail(result,
                    unsafe ? ROOTFS_WORK_PATH_UNSAFE :
                             ROOTFS_WORK_DESTINATION_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, error,
                    unsafe ? "destination directory traverses a reparse point"
                           : "cannot inspect destination directory");
        return false;
    }
    attributes = GetFileAttributesA(destination->destination_path);
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        result_fail(result,
                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u ?
                        ROOTFS_WORK_PATH_UNSAFE :
                        ROOTFS_WORK_DESTINATION_EXISTS,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                    "destination already exists");
        return false;
    }
    error = windows_error();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        result_fail(result, ROOTFS_WORK_DESTINATION_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, error,
                    "cannot establish that destination is absent");
        return false;
    }
    destination->handle = CreateFileA(destination->full_path,
                                      FILE_LIST_DIRECTORY,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      NULL, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS |
                                          FILE_FLAG_OPEN_REPARSE_POINT,
                                      NULL);
    if (destination->handle == INVALID_HANDLE_VALUE) {
        result_fail(result, ROOTFS_WORK_DESTINATION_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, windows_error(),
                    "cannot hold destination directory open");
        return false;
    }
    if (!windows_handle_matches_path(destination->handle,
                                     destination->full_path, &error)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, error,
                    "opened destination directory identity is ambiguous");
        return false;
    }
    return true;
}

static bool windows_open_source(const char *path, host_file_t *source,
                                file_stamp_t *stamp,
                                rootfs_work_result_t *result) {
    char *full = NULL;
    int error = 0;
    bool unsafe = false;
    BY_HANDLE_FILE_INFORMATION info;

    if (!windows_full_path(path, &full, &error)) {
        result_fail(result, error == ERROR_NOT_ENOUGH_MEMORY ?
                        ROOTFS_WORK_NO_MEMORY :
                        ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_PATH, error,
                    "cannot resolve source path");
        return false;
    }
    if (!windows_validate_chain(full, true, &error, &unsafe)) {
        result_fail(result,
                    unsafe ? ROOTFS_WORK_PATH_UNSAFE :
                             ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_PATH, error,
                    unsafe ? "source path traverses a reparse point"
                           : "cannot inspect source path");
        free(full);
        return false;
    }
    source->handle = CreateFileA(full, GENERIC_READ, FILE_SHARE_READ, NULL,
                                 OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL |
                                     FILE_FLAG_OPEN_REPARSE_POINT |
                                     FILE_FLAG_SEQUENTIAL_SCAN,
                                 NULL);
    if (source->handle == INVALID_HANDLE_VALUE) {
        error = windows_error();
        result_fail(result,
                    error == ERROR_SHARING_VIOLATION ?
                        ROOTFS_WORK_SOURCE_BUSY :
                        ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, error,
                    "cannot open source read-only with write/delete sharing denied");
        free(full);
        return false;
    }
    if (!windows_handle_matches_path(source->handle, full, &error)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, error,
                    "opened source identity is ambiguous");
        free(full);
        return false;
    }
    free(full);
    if (!GetFileInformationByHandle(source->handle, &info)) {
        result_fail(result, ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, windows_error(),
                    "cannot inspect opened source");
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, 0,
                    "opened source is a reparse point");
        return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) != 0u) {
        result_fail(result, ROOTFS_WORK_SOURCE_NOT_REGULAR,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, 0,
                    "source is not a regular disk image file");
        return false;
    }
    if (info.nNumberOfLinks != 1u) {
        result_fail(result, ROOTFS_WORK_SOURCE_ALIAS,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, 0,
                    "source has %lu hard links; immutable identity is ambiguous",
                    (unsigned long)info.nNumberOfLinks);
        return false;
    }
    if (!host_file_stamp(source, stamp, &error)) {
        result_fail(result, ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, error,
                    "cannot capture source identity");
        return false;
    }
    return true;
}

static bool destination_temp_create(destination_dir_t *destination,
                                     host_file_t *temporary,
                                     bool *temporary_created,
                                     rootfs_work_result_t *result) {
    unsigned attempt;
    size_t parent_length = strlen(destination->full_path);
    int identity_error = 0;

    if (!windows_handle_matches_path(destination->handle,
                                     destination->full_path,
                                     &identity_error)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_TEMP_CREATE, identity_error,
                    "destination directory identity changed before temp create");
        return false;
    }

    for (attempt = 0; attempt < ROOTFS_TEMP_ATTEMPTS; attempt++) {
        char leaf[80];
        int leaf_length = snprintf(leaf, sizeof(leaf),
                                   ".rootfs-work-%lu-%u.tmp",
                                   (unsigned long)_getpid(), attempt);
        size_t total;

        if (leaf_length <= 0 || (size_t)leaf_length >= sizeof(leaf))
            break;
        total = parent_length + 1u + (size_t)leaf_length + 1u;
        destination->temporary_path = (char *)malloc(total);
        if (!destination->temporary_path) {
            result_fail(result, ROOTFS_WORK_NO_MEMORY,
                        ROOTFS_WORK_STAGE_TEMP_CREATE, 0,
                        "cannot allocate temporary path");
            return false;
        }
        (void)snprintf(destination->temporary_path, total, "%s\\%s",
                       destination->full_path, leaf);
        if (_stricmp(destination->temporary_path,
                     destination->destination_path) == 0) {
            free(destination->temporary_path);
            destination->temporary_path = NULL;
            continue;
        }
        temporary->handle = CreateFileA(destination->temporary_path,
                                         GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (temporary->handle != INVALID_HANDLE_VALUE) {
            /* CREATE_NEW transferred ownership before any later identity
             * check can fail.  The caller's common cleanup path must own both
             * the handle and name from this instruction onward. */
            *temporary_created = true;
            if (windows_handle_matches_path(temporary->handle,
                                            destination->temporary_path,
                                            &identity_error))
                return true;
            result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                        ROOTFS_WORK_STAGE_TEMP_CREATE, identity_error,
                        "created temporary image identity is ambiguous");
            return false;
        }
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            int error = windows_error();
            result_fail(result, ROOTFS_WORK_TEMP_CREATE_FAILED,
                        ROOTFS_WORK_STAGE_TEMP_CREATE, error,
                        "cannot exclusively create temporary work image");
            return false;
        }
        free(destination->temporary_path);
        destination->temporary_path = NULL;
    }
    result_fail(result, ROOTFS_WORK_TEMP_CREATE_FAILED,
                ROOTFS_WORK_STAGE_TEMP_CREATE, ERROR_FILE_EXISTS,
                "all unique temporary names are occupied");
    return false;
}

#else /* !_WIN32 */

/* No syscall is allowed an unbounded retry loop: a signal storm must surface
 * as a stable EINTR failure rather than hanging a boot/provisioning process. */
static int posix_fstat_bounded(int descriptor, struct stat *info) {
    unsigned retries = 0;
    int rc;

    do {
        rc = fstat(descriptor, info);
    } while (rc != 0 && errno == EINTR &&
             retries++ < ROOTFS_EINTR_RETRY_LIMIT);
    return rc;
}

static int posix_fstatat_bounded(int descriptor, const char *path,
                                 struct stat *info, int flags) {
    unsigned retries = 0;
    int rc;

    do {
        rc = fstatat(descriptor, path, info, flags);
    } while (rc != 0 && errno == EINTR &&
             retries++ < ROOTFS_EINTR_RETRY_LIMIT);
    return rc;
}

static int posix_fsync_bounded(int descriptor) {
    unsigned retries = 0;
    int rc;

    do {
        rc = fsync(descriptor);
    } while (rc != 0 && errno == EINTR &&
             retries++ < ROOTFS_EINTR_RETRY_LIMIT);
    return rc;
}

static void host_file_init(host_file_t *file) {
    file->descriptor = -1;
}

static bool host_file_is_open(const host_file_t *file) {
    return file->descriptor >= 0;
}

static bool host_file_close(host_file_t *file, int *system_error) {
    int descriptor;

    if (!host_file_is_open(file))
        return true;
    descriptor = file->descriptor;
    file->descriptor = -1;
    if (close(descriptor) == 0)
        return true;
    *system_error = errno;
    return false;
}

static bool host_file_read(host_file_t *file, uint64_t offset, void *buffer,
                           size_t length, int *system_error) {
    uint8_t *next = (uint8_t *)buffer;

    if (offset > (uint64_t)INT64_MAX) {
        *system_error = EOVERFLOW;
        return false;
    }
    while (length != 0u) {
        size_t request = length;
        ssize_t actual;

#ifdef SSIZE_MAX
        if (request > (size_t)SSIZE_MAX)
            request = (size_t)SSIZE_MAX;
#endif
        unsigned retries = 0;

        do {
            actual = pread(file->descriptor, next, request, (off_t)offset);
        } while (actual < 0 && errno == EINTR &&
                 retries++ < ROOTFS_EINTR_RETRY_LIMIT);
        if (actual <= 0) {
            *system_error = actual == 0 ? EIO : errno;
            return false;
        }
        next += (size_t)actual;
        offset += (uint64_t)actual;
        length -= (size_t)actual;
    }
    return true;
}

static bool host_file_write(host_file_t *file, uint64_t offset,
                            const void *buffer, size_t length,
                            int *system_error) {
    const uint8_t *next = (const uint8_t *)buffer;

    if (offset > (uint64_t)INT64_MAX) {
        *system_error = EOVERFLOW;
        return false;
    }
    while (length != 0u) {
        size_t request = length;
        ssize_t actual;

#ifdef SSIZE_MAX
        if (request > (size_t)SSIZE_MAX)
            request = (size_t)SSIZE_MAX;
#endif
        unsigned retries = 0;

        do {
            actual = pwrite(file->descriptor, next, request, (off_t)offset);
        } while (actual < 0 && errno == EINTR &&
                 retries++ < ROOTFS_EINTR_RETRY_LIMIT);
        if (actual <= 0) {
            *system_error = actual == 0 ? EIO : errno;
            return false;
        }
        next += (size_t)actual;
        offset += (uint64_t)actual;
        length -= (size_t)actual;
    }
    return true;
}

static bool host_file_resize(host_file_t *file, uint64_t size,
                             int *system_error) {
    int rc;
    unsigned retries = 0;

    if (size > (uint64_t)INT64_MAX) {
        *system_error = EOVERFLOW;
        return false;
    }
    do {
        rc = ftruncate(file->descriptor, (off_t)size);
    } while (rc != 0 && errno == EINTR &&
             retries++ < ROOTFS_EINTR_RETRY_LIMIT);
    if (rc == 0)
        return true;
    *system_error = errno;
    return false;
}

static bool host_file_sync(host_file_t *file, int *system_error) {
    int rc = posix_fsync_bounded(file->descriptor);
    if (rc == 0)
        return true;
    *system_error = errno;
    return false;
}

static uint32_t links_to_u32(uintmax_t links) {
    return links > (uintmax_t)UINT32_MAX ? UINT32_MAX : (uint32_t)links;
}

static bool host_file_stamp(host_file_t *file, file_stamp_t *stamp,
                            int *system_error) {
    struct stat info;

    if (posix_fstat_bounded(file->descriptor, &info) != 0) {
        *system_error = errno;
        return false;
    }
    if (info.st_size < 0) {
        *system_error = EOVERFLOW;
        return false;
    }
    stamp->size = (uint64_t)info.st_size;
    stamp->identity_a = (uint64_t)info.st_dev;
    stamp->identity_b = (uint64_t)info.st_ino;
    stamp->modified_a = (uint64_t)info.st_mtime;
    stamp->changed_a = (uint64_t)info.st_ctime;
#if defined(__APPLE__) && defined(_DARWIN_C_SOURCE)
    stamp->modified_b = (uint64_t)info.st_mtimespec.tv_nsec;
    stamp->changed_b = (uint64_t)info.st_ctimespec.tv_nsec;
#elif defined(__APPLE__)
    /* Darwin's strict POSIX layout exposes nanoseconds as scalar fields. */
    stamp->modified_b = (uint64_t)info.st_mtimensec;
    stamp->changed_b = (uint64_t)info.st_ctimensec;
#elif defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
      defined(__OpenBSD__)
    stamp->modified_b = (uint64_t)info.st_mtim.tv_nsec;
    stamp->changed_b = (uint64_t)info.st_ctim.tv_nsec;
#else
    stamp->modified_b = 0;
    stamp->changed_b = 0;
#endif
    stamp->links = links_to_u32((uintmax_t)info.st_nlink);
    return true;
}

static int open_directory_no_links(const char *path, int *system_error,
                                   bool *unsafe) {
    char *copy = NULL;
    char *cursor;
    int current = -1;
    bool absolute;

    *unsafe = false;
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '/')) {
        *unsafe = true;
        return -1;
    }
    absolute = path[0] == '/';
    current = open(absolute ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (current < 0) {
        *system_error = errno;
        return -1;
    }
    copy = copy_string_n(path, strlen(path));
    if (!copy) {
        *system_error = ENOMEM;
        (void)close(current);
        return -1;
    }
    cursor = copy + (absolute ? 1 : 0);
    while (*cursor != '\0') {
        char *end;
        char saved;
        int next;
        struct stat before;
        struct stat after;
        int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;

        while (*cursor == '/')
            cursor++;
        if (*cursor == '\0')
            break;
        end = strchr(cursor, '/');
        if (!end)
            end = cursor + strlen(cursor);
        saved = *end;
        *end = '\0';
        if (strcmp(cursor, ".") == 0) {
            *end = saved;
            cursor = saved == '\0' ? end : end + 1;
            continue;
        }
        if (strcmp(cursor, "..") == 0) {
            *unsafe = true;
            *end = saved;
            free(copy);
            (void)close(current);
            return -1;
        }
        if (posix_fstatat_bounded(current, cursor, &before,
                                  AT_SYMLINK_NOFOLLOW) != 0) {
            *system_error = errno;
            *end = saved;
            free(copy);
            (void)close(current);
            return -1;
        }
        if (S_ISLNK(before.st_mode)) {
            *unsafe = true;
            *end = saved;
            free(copy);
            (void)close(current);
            return -1;
        }
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        next = openat(current, cursor, flags);
        if (next < 0) {
            *system_error = errno;
            *end = saved;
            free(copy);
            (void)close(current);
            return -1;
        }
        if (posix_fstat_bounded(next, &after) != 0) {
            *system_error = errno;
            (void)close(next);
            *end = saved;
            free(copy);
            (void)close(current);
            return -1;
        }
        if (!S_ISDIR(after.st_mode) || before.st_dev != after.st_dev ||
            before.st_ino != after.st_ino) {
            *system_error = 0;
            *unsafe = true;
            (void)close(next);
            *end = saved;
            free(copy);
            (void)close(current);
            return -1;
        }
        (void)close(current);
        current = next;
        *end = saved;
        cursor = saved == '\0' ? end : end + 1;
    }
    free(copy);
    return current;
}

static bool split_path(const char *path, char **parent, char **leaf) {
    const char *slash;
    size_t parent_length;

    if (!path || path[0] == '\0' || path[strlen(path) - 1u] == '/')
        return false;
    slash = strrchr(path, '/');
    if (!slash) {
        *parent = copy_string_n(".", 1u);
        *leaf = copy_string_n(path, strlen(path));
    } else {
        parent_length = slash == path ? 1u : (size_t)(slash - path);
        *parent = copy_string_n(path, parent_length);
        *leaf = copy_string_n(slash + 1, strlen(slash + 1));
    }
    if (!*parent || !*leaf || (*leaf)[0] == '\0' ||
        strcmp(*leaf, ".") == 0 || strcmp(*leaf, "..") == 0) {
        free(*parent);
        free(*leaf);
        *parent = NULL;
        *leaf = NULL;
        return false;
    }
    return true;
}

static bool posix_open_source(const char *path, host_file_t *source,
                              file_stamp_t *stamp,
                              rootfs_work_result_t *result) {
    char *parent = NULL;
    char *leaf = NULL;
    int parent_fd = -1;
    int error = 0;
    bool unsafe = false;
    struct stat before;
    struct stat after;
    struct flock lock;
    int flags = O_RDONLY | O_CLOEXEC;

    if (!split_path(path, &parent, &leaf)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_SOURCE_PATH, 0,
                    "source has no safe file name");
        return false;
    }
    parent_fd = open_directory_no_links(parent, &error, &unsafe);
    if (parent_fd < 0) {
        result_fail(result,
                    unsafe ? ROOTFS_WORK_PATH_UNSAFE :
                             (error == ENOMEM ? ROOTFS_WORK_NO_MEMORY :
                                                ROOTFS_WORK_SOURCE_OPEN_FAILED),
                    ROOTFS_WORK_STAGE_SOURCE_PATH, error,
                    unsafe ? "source path traverses a symbolic link or '..'"
                           : "cannot open source directory");
        free(parent);
        free(leaf);
        return false;
    }
    if (posix_fstatat_bounded(parent_fd, leaf, &before,
                              AT_SYMLINK_NOFOLLOW) != 0) {
        error = errno;
        result_fail(result, ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_PATH, error,
                    "cannot inspect source path");
        (void)close(parent_fd);
        free(parent);
        free(leaf);
        return false;
    }
    if (S_ISLNK(before.st_mode)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_SOURCE_PATH, 0,
                    "source is a symbolic link");
        (void)close(parent_fd);
        free(parent);
        free(leaf);
        return false;
    }
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    source->descriptor = openat(parent_fd, leaf, flags);
    error = errno;
    (void)close(parent_fd);
    free(parent);
    free(leaf);
    if (source->descriptor < 0) {
        result_fail(result, ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, error,
                    "cannot open source read-only");
        return false;
    }
    if (posix_fstat_bounded(source->descriptor, &after) != 0) {
        result_fail(result, ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, errno,
                    "cannot inspect opened source");
        return false;
    }
    if (before.st_dev != after.st_dev || before.st_ino != after.st_ino) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, 0,
                    "source identity changed while it was opened");
        return false;
    }
    if (!S_ISREG(after.st_mode)) {
        result_fail(result, ROOTFS_WORK_SOURCE_NOT_REGULAR,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, 0,
                    "source is not a regular disk image file");
        return false;
    }
    if (after.st_nlink != 1) {
        result_fail(result, ROOTFS_WORK_SOURCE_ALIAS,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, 0,
                    "source has %lu hard links; immutable identity is ambiguous",
                    (unsigned long)after.st_nlink);
        return false;
    }
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_RDLCK;
    lock.l_whence = SEEK_SET;
    {
        unsigned retries = 0;
        int rc;

        do {
            rc = fcntl(source->descriptor, F_SETLK, &lock);
        } while (rc != 0 && errno == EINTR &&
                 retries++ < ROOTFS_EINTR_RETRY_LIMIT);
        if (rc != 0) {
        error = errno;
        result_fail(result, ROOTFS_WORK_SOURCE_BUSY,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, error,
                    "cannot lock source against cooperative writers");
        return false;
        }
    }
    if (!host_file_stamp(source, stamp, &error)) {
        result_fail(result, ROOTFS_WORK_SOURCE_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_SOURCE_OPEN, error,
                    "cannot capture source identity");
        return false;
    }
    return true;
}

static bool posix_open_destination(const char *path,
                                   destination_dir_t *destination,
                                   rootfs_work_result_t *result) {
    char *parent = NULL;
    int error = 0;
    bool unsafe = false;
    struct stat entry;

    if (!split_path(path, &parent, &destination->leaf)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                    "destination has no safe file name");
        return false;
    }
    destination->descriptor = open_directory_no_links(parent, &error, &unsafe);
    free(parent);
    if (destination->descriptor < 0) {
        result_fail(result,
                    unsafe ? ROOTFS_WORK_PATH_UNSAFE :
                             (error == ENOMEM ? ROOTFS_WORK_NO_MEMORY :
                                                ROOTFS_WORK_DESTINATION_OPEN_FAILED),
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, error,
                    unsafe ? "destination path traverses a symbolic link or '..'"
                           : "cannot open destination directory");
        return false;
    }
    if (posix_fstatat_bounded(destination->descriptor, destination->leaf,
                              &entry, AT_SYMLINK_NOFOLLOW) == 0) {
        result_fail(result,
                    S_ISLNK(entry.st_mode) ? ROOTFS_WORK_PATH_UNSAFE :
                                             ROOTFS_WORK_DESTINATION_EXISTS,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, 0,
                    "destination already exists");
        return false;
    }
    if (errno != ENOENT) {
        result_fail(result, ROOTFS_WORK_DESTINATION_OPEN_FAILED,
                    ROOTFS_WORK_STAGE_DESTINATION_PATH, errno,
                    "cannot establish that destination is absent");
        return false;
    }
    return true;
}

static bool destination_temp_create(destination_dir_t *destination,
                                     host_file_t *temporary,
                                     bool *temporary_created,
                                     rootfs_work_result_t *result) {
    unsigned attempt;

    for (attempt = 0; attempt < ROOTFS_TEMP_ATTEMPTS; attempt++) {
        int length = snprintf(destination->temporary_leaf,
                              sizeof(destination->temporary_leaf),
                              ".rootfs-work-%lu-%u.tmp",
                              (unsigned long)getpid(), attempt);
        int flags = O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC;

        if (length <= 0 || (size_t)length >= sizeof(destination->temporary_leaf))
            break;
        if (strcmp(destination->temporary_leaf, destination->leaf) == 0)
            continue;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        temporary->descriptor = openat(destination->descriptor,
                                       destination->temporary_leaf,
                                       flags, (mode_t)0600);
        if (temporary->descriptor >= 0) {
            /* openat(O_CREAT|O_EXCL) transferred ownership even if a future
             * post-create validation is added before this function returns. */
            *temporary_created = true;
            return true;
        }
        if (errno != EEXIST) {
            result_fail(result, ROOTFS_WORK_TEMP_CREATE_FAILED,
                        ROOTFS_WORK_STAGE_TEMP_CREATE, errno,
                        "cannot exclusively create temporary work image");
            return false;
        }
    }
    result_fail(result, ROOTFS_WORK_TEMP_CREATE_FAILED,
                ROOTFS_WORK_STAGE_TEMP_CREATE, EEXIST,
                "all unique temporary names are occupied");
    return false;
}

#endif /* _WIN32 */

static bool range_valid(uint64_t offset, size_t length, uint64_t size) {
    return offset <= size && (uint64_t)length <= size - offset;
}

static bool checked_read(host_file_t *file, uint64_t file_size, uint64_t offset,
                         void *buffer, size_t length,
                         rootfs_work_stage_t stage,
                         rootfs_work_result_t *result) {
    int error = 0;

    if (!range_valid(offset, length, file_size)) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR, stage, 0,
                    "read [0x%" PRIx64 ",+%zu) exceeds 0x%" PRIx64 " bytes",
                    offset, length, file_size);
        return false;
    }
    if (!host_file_read(file, offset, buffer, length, &error)) {
        result_fail(result, ROOTFS_WORK_READ_FAILED, stage, error,
                    "positioned read failed at 0x%" PRIx64 " for %zu bytes",
                    offset, length);
        return false;
    }
    return true;
}

static bool checked_write(host_file_t *file, uint64_t file_size,
                          uint64_t offset, const void *buffer, size_t length,
                          rootfs_work_stage_t stage,
                          rootfs_work_result_t *result) {
    int error = 0;

    if (!range_valid(offset, length, file_size)) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR, stage, 0,
                    "write [0x%" PRIx64 ",+%zu) exceeds 0x%" PRIx64 " bytes",
                    offset, length, file_size);
        return false;
    }
    if (!host_file_write(file, offset, buffer, length, &error)) {
        result_fail(result, ROOTFS_WORK_WRITE_FAILED, stage, error,
                    "positioned write failed at 0x%" PRIx64 " for %zu bytes",
                    offset, length);
        return false;
    }
    return true;
}

static bool allocation_physical_offset(const hfs_volume_t *volume,
                                       uint64_t logical_offset,
                                       uint64_t *physical_offset) {
    uint64_t seen = 0;
    unsigned index;

    for (index = 0; index < 8u; index++) {
        uint64_t span = (uint64_t)volume->ext_count[index] *
                        volume->block_size;
        if (span == 0u)
            continue;
        if (logical_offset < seen + span) {
            *physical_offset =
                (uint64_t)volume->ext_start[index] * volume->block_size +
                (logical_offset - seen);
            return true;
        }
        seen += span;
    }
    return false;
}

static uint32_t hfs_head_end(uint32_t block_size) {
    return (uint32_t)((1536u + block_size - 1u) / block_size);
}

static uint32_t hfs_tail_first(uint32_t total_blocks, uint32_t block_size) {
    return (uint32_t)(((uint64_t)total_blocks * block_size - HFS_VH_OFF) /
                      block_size);
}

static bool allocation_file_contains_block(const hfs_volume_t *volume,
                                           uint32_t block) {
    unsigned extent;

    for (extent = 0; extent < 8u; extent++) {
        uint64_t end = (uint64_t)volume->ext_start[extent] +
                       volume->ext_count[extent];
        if (volume->ext_count[extent] != 0u &&
            block >= volume->ext_start[extent] && block < end)
            return true;
    }
    return false;
}

static bool allocation_scan(host_file_t *file, uint64_t file_size,
                            const hfs_volume_t *volume, uint8_t *buffer,
                            size_t buffer_size, uint32_t *used,
                            rootfs_work_stage_t stage,
                            rootfs_work_result_t *result) {
    uint64_t logical = 0;
    uint64_t remaining = volume->alloc_bytes;
    uint64_t used_count = 0;
    uint32_t head_end = hfs_head_end(volume->block_size);
    uint32_t tail_first = hfs_tail_first(volume->total_blocks,
                                         volume->block_size);
    unsigned extent;

    for (extent = 0; extent < 8u && remaining != 0u; extent++) {
        uint64_t extent_bytes = (uint64_t)volume->ext_count[extent] *
                                volume->block_size;
        uint64_t in_extent = extent_bytes < remaining ? extent_bytes : remaining;
        uint64_t physical = (uint64_t)volume->ext_start[extent] *
                            volume->block_size;

        while (in_extent != 0u) {
            size_t amount = in_extent > buffer_size ? buffer_size :
                            (size_t)in_extent;
            size_t byte_index;

            if (!checked_read(file, file_size, physical, buffer, amount,
                              stage, result))
                return false;
            for (byte_index = 0; byte_index < amount; byte_index++) {
                uint8_t value = buffer[byte_index];
                unsigned bit_in_byte;

                for (bit_in_byte = 0; bit_in_byte < 8u; bit_in_byte++) {
                    uint64_t bit = (logical + byte_index) * 8u + bit_in_byte;
                    bool set = (value & (uint8_t)(1u <<
                                      (7u - bit_in_byte))) != 0u;
                    if (bit < volume->total_blocks) {
                        bool required = bit < head_end || bit >= tail_first ||
                            allocation_file_contains_block(volume,
                                                           (uint32_t)bit);
                        if (required && !set) {
                            result_fail(result, ROOTFS_WORK_HFS_INVALID, stage,
                                        0, "required metadata block %" PRIu64
                                        " is marked free", bit);
                            return false;
                        }
                        if (set)
                            used_count++;
                    } else if (set) {
                        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                                    "allocation bit %" PRIu64
                                    " is set past totalBlocks %u",
                                    bit, volume->total_blocks);
                        return false;
                    }
                }
            }
            logical += amount;
            physical += amount;
            in_extent -= amount;
            remaining -= amount;
        }
    }
    if (remaining != 0u || logical != volume->alloc_bytes) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "allocation fork extents do not cover logicalSize");
        return false;
    }
    if (used_count > UINT32_MAX) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "allocation bitmap used-block count overflows");
        return false;
    }
    *used = (uint32_t)used_count;
    return true;
}

static bool hfs_validate(host_file_t *file, uint64_t file_size,
                         hfs_volume_t *volume, uint8_t *buffer,
                         size_t buffer_size, rootfs_work_stage_t stage,
                         rootfs_work_result_t *result) {
    uint8_t primary[HFS_VH_LEN];
    uint8_t alternate[HFS_VH_LEN];
    uint16_t signature;
    uint16_t version;
    uint32_t journal_info_block;
    uint64_t summed = 0;
    uint32_t used = 0;
    unsigned index;

    memset(volume, 0, sizeof(*volume));
    if (file_size < HFS_VH_OFF + HFS_VH_LEN) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "image is too small for an HFS volume header");
        return false;
    }
    if (!checked_read(file, file_size, HFS_VH_OFF, primary,
                      sizeof(primary), stage, result))
        return false;
    signature = read_be16(primary);
    version = read_be16(primary + 2);
    if (!((signature == HFS_SIG_HFSPLUS && version == 4u) ||
          (signature == HFS_SIG_HFSX && version == 5u))) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "signature 0x%04x/version %u is not HFS+/4 or HFSX/5",
                    signature, version);
        return false;
    }
    volume->attributes = read_be32(primary + 4);
    journal_info_block = read_be32(primary + 12);
    volume->block_size = read_be32(primary + 40);
    volume->total_blocks = read_be32(primary + 44);
    volume->free_blocks = read_be32(primary + 48);
    volume->next_alloc = read_be32(primary + 52);
    if (volume->block_size < 512u || volume->block_size > (1u << 20) ||
        (volume->block_size & (volume->block_size - 1u)) != 0u ||
        volume->block_size % 512u != 0u) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "blockSize %u is not a power-of-two multiple of 512",
                    volume->block_size);
        return false;
    }
    if (volume->total_blocks == 0u ||
        (uint64_t)volume->total_blocks * volume->block_size != file_size) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "totalBlocks %u x blockSize %u does not equal image size %"
                    PRIu64, volume->total_blocks, volume->block_size, file_size);
        return false;
    }
    if (volume->free_blocks > volume->total_blocks) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "freeBlocks %u exceeds totalBlocks %u",
                    volume->free_blocks, volume->total_blocks);
        return false;
    }
    if (volume->next_alloc >= volume->total_blocks) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "nextAllocation %u is outside %u blocks",
                    volume->next_alloc, volume->total_blocks);
        return false;
    }
    if ((volume->attributes & HFS_ATTR_JOURNALED) != 0u ||
        journal_info_block != 0u) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "journalled volumes are not supported");
        return false;
    }
    if ((volume->attributes & HFS_ATTR_SOFTWARE_LOCK) != 0u) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "software-locked volumes cannot be made writable");
        return false;
    }
    if ((volume->attributes & HFS_ATTR_UNMOUNTED) == 0u) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "source volume was not cleanly unmounted");
        return false;
    }
    if ((volume->attributes & HFS_ATTR_BOOT_INCONSISTENT) != 0u) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "source volume carries the boot-inconsistent bit");
        return false;
    }
    if (!checked_read(file, file_size, file_size - HFS_VH_OFF, alternate,
                      sizeof(alternate), stage, result))
        return false;
    if (memcmp(primary, alternate, sizeof(primary)) != 0) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "primary and alternate volume headers disagree");
        return false;
    }

    volume->alloc_bytes = read_be64(primary + 112);
    volume->alloc_fork_blocks = read_be32(primary + 124);
    {
        bool saw_empty_extent = false;

    for (index = 0; index < 8u; index++) {
        uint64_t extent_end;
        unsigned prior;

        volume->ext_start[index] = read_be32(primary + 128 + index * 8u);
        volume->ext_count[index] = read_be32(primary + 132 + index * 8u);
        if (volume->ext_count[index] == 0u) {
            if (volume->ext_start[index] != 0u) {
                result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                            "empty allocation extent %u has nonzero startBlock",
                            index);
                return false;
            }
            saw_empty_extent = true;
            continue;
        }
        if (saw_empty_extent) {
            result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                        "allocation extent %u follows an empty inline extent",
                        index);
            return false;
        }
        extent_end = (uint64_t)volume->ext_start[index] +
                     volume->ext_count[index];
        if (extent_end > volume->total_blocks) {
            result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                        "allocation extent %u runs past the volume", index);
            return false;
        }
        for (prior = 0; prior < index; prior++) {
            uint64_t prior_end = (uint64_t)volume->ext_start[prior] +
                                 volume->ext_count[prior];
            if (volume->ext_count[prior] != 0u &&
                volume->ext_start[index] < prior_end &&
                volume->ext_start[prior] < extent_end) {
                result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                            "allocation extents %u and %u overlap",
                            prior, index);
                return false;
            }
        }
        summed += volume->ext_count[index];
    }
    }
    if (summed != volume->alloc_fork_blocks) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "allocation fork reports %u blocks but inline extents cover "
                    "%" PRIu64, volume->alloc_fork_blocks, summed);
        return false;
    }
    if (volume->alloc_bytes >
        (uint64_t)volume->alloc_fork_blocks * volume->block_size) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "allocation logicalSize exceeds its physical extents");
        return false;
    }
    if (volume->alloc_bytes > UINT32_MAX / 8u) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "allocation bitmap is too large for 32-bit HFS blocks");
        return false;
    }
    volume->nbits = (uint32_t)(volume->alloc_bytes * 8u);
    if (volume->nbits < volume->total_blocks) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "allocation bitmap has %u bits for %u blocks",
                    volume->nbits, volume->total_blocks);
        return false;
    }
    if (!allocation_scan(file, file_size, volume, buffer, buffer_size,
                         &used, stage, result))
        return false;
    if (used != volume->total_blocks - volume->free_blocks) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID, stage, 0,
                    "bitmap marks %u used blocks but header implies %u",
                    used, volume->total_blocks - volume->free_blocks);
        return false;
    }
    return true;
}

static bool allocation_bit_read(host_file_t *file, uint64_t file_size,
                                const hfs_volume_t *volume, uint32_t bit,
                                bool *set, rootfs_work_stage_t stage,
                                rootfs_work_result_t *result) {
    uint64_t offset;
    uint8_t byte;

    if (bit >= volume->nbits ||
        !allocation_physical_offset(volume, bit >> 3, &offset)) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR, stage, 0,
                    "allocation bit %u has no mapped byte", bit);
        return false;
    }
    if (!checked_read(file, file_size, offset, &byte, 1u, stage, result))
        return false;
    *set = (byte & (uint8_t)(1u << (7u - (bit & 7u)))) != 0u;
    return true;
}

static bool allocation_bit_write(host_file_t *file, uint64_t file_size,
                                 const hfs_volume_t *volume, uint32_t bit,
                                 bool set, rootfs_work_stage_t stage,
                                 rootfs_work_result_t *result) {
    uint64_t offset;
    uint8_t byte;
    uint8_t mask;

    if (bit >= volume->nbits ||
        !allocation_physical_offset(volume, bit >> 3, &offset)) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR, stage, 0,
                    "allocation bit %u has no mapped byte", bit);
        return false;
    }
    if (!checked_read(file, file_size, offset, &byte, 1u, stage, result))
        return false;
    mask = (uint8_t)(1u << (7u - (bit & 7u)));
    byte = set ? (uint8_t)(byte | mask) : (uint8_t)(byte & (uint8_t)~mask);
    return checked_write(file, file_size, offset, &byte, 1u, stage, result);
}

static bool fstab_rewrite(host_file_t *file, uint64_t file_size,
                          const char *line, uint8_t *buffer,
                          size_t buffer_size, rootfs_work_result_t *result) {
    size_t prefix[sizeof(FSTAB_STOCK) - 1u];
    const size_t pattern_size = sizeof(FSTAB_STOCK) - 1u;
    uint64_t offset = 0;
    uint64_t hit = UINT64_MAX;
    unsigned hits = 0;
    size_t matched = 0;
    size_t index;
    size_t line_size = 0;
    uint8_t replacement[sizeof(FSTAB_STOCK) - 1u];

    if (!line) {
        result_fail(result, ROOTFS_WORK_FSTAB_LINE_INVALID,
                    ROOTFS_WORK_STAGE_FSTAB_WRITE, 0,
                    "fstab replacement line is NULL");
        return false;
    }
    while (line_size < pattern_size && line[line_size] != '\0') {
        if (line[line_size] == '\n' || line[line_size] == '\r') {
            result_fail(result, ROOTFS_WORK_FSTAB_LINE_INVALID,
                        ROOTFS_WORK_STAGE_FSTAB_WRITE, 0,
                        "fstab replacement must be exactly one line");
            return false;
        }
        line_size++;
    }
    if (line_size == 0u || line_size == pattern_size ||
        line_size + 1u > pattern_size) {
        result_fail(result, ROOTFS_WORK_FSTAB_LINE_INVALID,
                    ROOTFS_WORK_STAGE_FSTAB_WRITE, 0,
                    "fstab replacement must contain 1..%zu bytes",
                    pattern_size - 1u);
        return false;
    }

    prefix[0] = 0u;
    for (index = 1u; index < pattern_size; index++) {
        size_t candidate = prefix[index - 1u];
        while (candidate != 0u &&
               FSTAB_STOCK[index] != FSTAB_STOCK[candidate])
            candidate = prefix[candidate - 1u];
        if (FSTAB_STOCK[index] == FSTAB_STOCK[candidate])
            candidate++;
        prefix[index] = candidate;
    }
    while (offset < file_size) {
        uint64_t remaining = file_size - offset;
        size_t amount = remaining > buffer_size ? buffer_size :
                        (size_t)remaining;

        if (!checked_read(file, file_size, offset, buffer, amount,
                          ROOTFS_WORK_STAGE_FSTAB_SCAN, result))
            return false;
        for (index = 0; index < amount; index++) {
            uint8_t value = buffer[index];
            while (matched != 0u && value != FSTAB_STOCK[matched])
                matched = prefix[matched - 1u];
            if (value == FSTAB_STOCK[matched])
                matched++;
            if (matched == pattern_size) {
                uint64_t end = offset + index + 1u;
                hit = end - pattern_size;
                hits++;
                if (hits > 1u) {
                    result_fail(result, ROOTFS_WORK_FSTAB_NOT_UNIQUE,
                                ROOTFS_WORK_STAGE_FSTAB_SCAN, 0,
                                "stock fstab record occurs more than once");
                    return false;
                }
                matched = prefix[matched - 1u];
            }
        }
        offset += amount;
    }
    if (hits != 1u) {
        result_fail(result, ROOTFS_WORK_FSTAB_NOT_UNIQUE,
                    ROOTFS_WORK_STAGE_FSTAB_SCAN, 0,
                    "stock fstab record occurs 0 times; exactly 1 is required");
        return false;
    }
    memcpy(replacement, line, line_size);
    replacement[line_size] = '\n';
    if (pattern_size > line_size + 1u) {
        size_t padding = pattern_size - line_size - 1u;
        if (padding > 1u)
            memset(replacement + line_size + 1u, '#', padding - 1u);
        replacement[pattern_size - 1u] = '\n';
    }
    if (!checked_write(file, file_size, hit, replacement, sizeof(replacement),
                       ROOTFS_WORK_STAGE_FSTAB_WRITE, result))
        return false;
    result->fstab_offset = hit;
    return true;
}

static bool extent_overlaps(uint32_t start, uint32_t count,
                            uint32_t range_start, uint32_t range_end) {
    uint64_t end = (uint64_t)start + count;
    return count != 0u && start < range_end && range_start < end;
}

static bool grow_volume(host_file_t *file, uint64_t *file_size,
                        uint64_t growth_bytes, const hfs_volume_t *before,
                        uint8_t *buffer, size_t buffer_size,
                        rootfs_work_result_t *result) {
    uint64_t requested_blocks;
    uint64_t requested_total;
    uint32_t new_total;
    uint32_t old_tail;
    uint32_t new_tail;
    uint64_t new_size;
    uint8_t primary[HFS_VH_LEN];
    uint32_t used;
    unsigned index;
    int error = 0;

    if (growth_bytes == 0u)
        return true;
    requested_blocks = growth_bytes / before->block_size;
    if (requested_blocks != 0u)
        requested_blocks--;
    requested_total = (uint64_t)before->total_blocks + requested_blocks;
    new_total = requested_total > before->nbits ? before->nbits :
                (uint32_t)requested_total;
    if (new_total <= before->total_blocks) {
        result_fail(result, ROOTFS_WORK_GROW_INVALID,
                    ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                    "growth request is less than two allocation blocks or bitmap is full");
        return false;
    }
    old_tail = hfs_tail_first(before->total_blocks, before->block_size);
    new_tail = hfs_tail_first(new_total, before->block_size);
    if (hfs_head_end(before->block_size) > old_tail ||
        hfs_head_end(before->block_size) > new_tail) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID,
                    ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                    "reserved head and tail allocation-block ranges overlap");
        return false;
    }
    if (old_tail >= before->total_blocks || new_tail >= new_total) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID,
                    ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                    "reserved-tail block range is invalid");
        return false;
    }
    for (index = 0; index < 8u; index++) {
        if (extent_overlaps(before->ext_start[index], before->ext_count[index],
                            old_tail, before->total_blocks)) {
            result_fail(result, ROOTFS_WORK_HFS_INVALID,
                        ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                        "allocation file overlaps the old reserved tail");
            return false;
        }
    }
    for (index = old_tail; index < before->total_blocks; index++) {
        bool set;
        if (!allocation_bit_read(file, *file_size, before, index, &set,
                                 ROOTFS_WORK_STAGE_GROW_PLAN, result))
            return false;
        if (!set) {
            result_fail(result, ROOTFS_WORK_HFS_INVALID,
                        ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                        "old reserved-tail block %u is marked free", index);
            return false;
        }
    }
    for (index = new_tail; index < new_total; index++) {
        bool set;
        if (index < before->total_blocks)
            continue;
        if (!allocation_bit_read(file, *file_size, before, index, &set,
                                 ROOTFS_WORK_STAGE_GROW_PLAN, result))
            return false;
        if (set) {
            result_fail(result, ROOTFS_WORK_HFS_INVALID,
                        ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                        "new reserved-tail block %u is already allocated",
                        index);
            return false;
        }
    }
    new_size = (uint64_t)new_total * before->block_size;
    if (new_size <= *file_size || new_size > (uint64_t)INT64_MAX) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR,
                    ROOTFS_WORK_STAGE_GROW_PLAN, 0,
                    "grown image size is outside the supported 64-bit range");
        return false;
    }
    if (!host_file_resize(file, new_size, &error)) {
        result_fail(result, ROOTFS_WORK_WRITE_FAILED,
                    ROOTFS_WORK_STAGE_GROW_WRITE, error,
                    "cannot extend temporary work image to %" PRIu64 " bytes",
                    new_size);
        return false;
    }
    *file_size = new_size;
    for (index = old_tail; index < before->total_blocks; index++) {
        if (!allocation_bit_write(file, *file_size, before, index, false,
                                  ROOTFS_WORK_STAGE_GROW_WRITE, result))
            return false;
    }
    for (index = new_tail; index < new_total; index++) {
        if (!allocation_bit_write(file, *file_size, before, index, true,
                                  ROOTFS_WORK_STAGE_GROW_WRITE, result))
            return false;
    }

    {
        hfs_volume_t recount = *before;
        recount.total_blocks = new_total;
        recount.nbits = before->nbits;
        if (!allocation_scan(file, *file_size, &recount, buffer, buffer_size,
                             &used, ROOTFS_WORK_STAGE_GROW_WRITE, result))
            return false;
    }
    if (!checked_read(file, *file_size, HFS_VH_OFF, primary, sizeof(primary),
                      ROOTFS_WORK_STAGE_GROW_WRITE, result))
        return false;
    write_be32(primary + 44, new_total);
    write_be32(primary + 48, new_total - used);
    write_be32(primary + 52, old_tail);
    if (!checked_write(file, *file_size, HFS_VH_OFF, primary, sizeof(primary),
                       ROOTFS_WORK_STAGE_GROW_WRITE, result) ||
        !checked_write(file, *file_size, new_size - HFS_VH_OFF, primary,
                       sizeof(primary), ROOTFS_WORK_STAGE_GROW_WRITE, result))
        return false;
    return true;
}

static bool copy_source(host_file_t *source, host_file_t *temporary,
                        uint64_t source_size, uint8_t *buffer,
                        size_t buffer_size,
                        ios3_sha256_context_t *source_sha256,
                        rootfs_work_result_t *result) {
    uint64_t offset = 0;

    while (offset < source_size) {
        uint64_t remaining = source_size - offset;
        size_t amount = remaining > buffer_size ? buffer_size :
                        (size_t)remaining;

        if (!checked_read(source, source_size, offset, buffer, amount,
                          ROOTFS_WORK_STAGE_COPY, result))
            return false;
        if (!ios3_sha256_update(source_sha256, buffer, amount)) {
            result_fail(result, ROOTFS_WORK_RANGE_ERROR,
                        ROOTFS_WORK_STAGE_SOURCE_IDENTITY, 0,
                        "source exceeds the SHA-256 cumulative length bound");
            return false;
        }
        if (!checked_write(temporary, source_size, offset, buffer, amount,
                           ROOTFS_WORK_STAGE_COPY, result))
            return false;
        offset += amount;
        result->bytes_copied = offset;
    }
    return true;
}

static void destination_init(destination_dir_t *destination) {
    memset(destination, 0, sizeof(*destination));
#ifdef _WIN32
    destination->handle = INVALID_HANDLE_VALUE;
#else
    destination->descriptor = -1;
    destination->temporary_leaf[0] = '\0';
#endif
}

static void destination_release(destination_dir_t *destination) {
#ifdef _WIN32
    if (destination->handle != INVALID_HANDLE_VALUE)
        (void)CloseHandle(destination->handle);
    free(destination->full_path);
    free(destination->destination_path);
    free(destination->temporary_path);
#else
    if (destination->descriptor >= 0)
        (void)close(destination->descriptor);
    free(destination->leaf);
#endif
    destination_init(destination);
}

static void cleanup_unpublished(destination_dir_t *destination,
                                 bool temporary_created,
                                 rootfs_work_result_t *result) {
    if (!temporary_created || result->published)
        return;
#ifdef _WIN32
    if (destination->temporary_path &&
        !DeleteFileA(destination->temporary_path)) {
        if (result->cleanup_system_error == 0)
            result->cleanup_system_error = windows_error();
        result->temporary_left = true;
    }
#else
    if (destination->descriptor >= 0 &&
        destination->temporary_leaf[0] != '\0' &&
        unlinkat(destination->descriptor, destination->temporary_leaf, 0) != 0) {
        if (result->cleanup_system_error == 0)
            result->cleanup_system_error = errno;
        result->temporary_left = true;
    }
#endif
}

static bool publish(destination_dir_t *destination,
                    rootfs_work_result_t *result) {
#ifdef _WIN32
    int identity_error = 0;

    /* MoveFileEx is path-based.  Keep the final directory open without delete
     * sharing and revalidate its canonical handle identity immediately before
     * the move; this detects ordinary ancestor/reparse renames.  It is not
     * presented as a security boundary against a hostile same-user namespace
     * racer, which documented Win32 lacks an openat-style primitive to close. */
    if (!windows_handle_matches_path(destination->handle,
                                     destination->full_path,
                                     &identity_error)) {
        result_fail(result, ROOTFS_WORK_PATH_UNSAFE,
                    ROOTFS_WORK_STAGE_PUBLISH, identity_error,
                    "destination directory identity changed before publish");
        return false;
    }
    if (!MoveFileExA(destination->temporary_path,
                     destination->destination_path, MOVEFILE_WRITE_THROUGH)) {
        int error = windows_error();
        result_fail(result,
                    error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS ?
                        ROOTFS_WORK_DESTINATION_EXISTS :
                        ROOTFS_WORK_PUBLISH_FAILED,
                    ROOTFS_WORK_STAGE_PUBLISH, error,
                    "atomic no-replace publication failed");
        return false;
    }
    result->published = true;
    return true;
#else
    if (linkat(destination->descriptor, destination->temporary_leaf,
               destination->descriptor, destination->leaf, 0) != 0) {
        int error = errno;
        result_fail(result,
                    error == EEXIST ? ROOTFS_WORK_DESTINATION_EXISTS :
                                      ROOTFS_WORK_PUBLISH_FAILED,
                    ROOTFS_WORK_STAGE_PUBLISH, error,
                    "atomic no-replace publication failed");
        return false;
    }
    result->published = true;
    if (posix_fsync_bounded(destination->descriptor) != 0) {
        result->temporary_left = true;
        result_fail(result, ROOTFS_WORK_PUBLISH_DURABILITY_FAILED,
                    ROOTFS_WORK_STAGE_DIRECTORY_SYNC, errno,
                    "destination was published but directory sync failed");
        return false;
    }
    if (unlinkat(destination->descriptor, destination->temporary_leaf, 0) != 0) {
        result->temporary_left = true;
        result_fail(result, ROOTFS_WORK_PUBLISH_FAILED,
                    ROOTFS_WORK_STAGE_CLEANUP, errno,
                    "destination was published but temporary link removal failed");
        return false;
    }
    if (posix_fsync_bounded(destination->descriptor) != 0) {
        result_fail(result, ROOTFS_WORK_PUBLISH_DURABILITY_FAILED,
                    ROOTFS_WORK_STAGE_DIRECTORY_SYNC, errno,
                    "destination is durable but temporary-link removal sync failed");
        return false;
    }
    return true;
#endif
}

rootfs_work_status_t rootfs_work_create(const char *source_path,
                                        const char *destination_path,
                                        const rootfs_work_options_t *options,
                                        rootfs_work_result_t *result) {
    rootfs_work_options_t selected;
    host_file_t source;
    host_file_t temporary;
    destination_dir_t destination;
    file_stamp_t source_before;
    file_stamp_t source_after;
    file_stamp_t temporary_stamp;
    hfs_volume_t source_volume;
    hfs_volume_t copied_volume;
    hfs_volume_t final_volume;
    uint8_t *buffer = NULL;
    ios3_sha256_context_t source_sha256;
    uint64_t work_size = 0;
    bool temporary_created = false;
    bool success = false;
    int error = 0;

    if (!result)
        return ROOTFS_WORK_INVALID_ARGUMENT;
    result_reset(result);
    host_file_init(&source);
    host_file_init(&temporary);
    destination_init(&destination);
    memset(&selected, 0, sizeof(selected));
    if (options)
        selected = *options;
    if (!selected.fstab_line)
        selected.fstab_line = ROOTFS_WORK_DEFAULT_FSTAB;
    if (selected.io_buffer_bytes == 0u)
        selected.io_buffer_bytes = ROOTFS_WORK_MAX_IO_BUFFER;
    if (!source_path || source_path[0] == '\0' || !destination_path ||
        destination_path[0] == '\0' || selected.io_buffer_bytes == 0u ||
        selected.io_buffer_bytes > ROOTFS_WORK_MAX_IO_BUFFER) {
        return result_fail(result, ROOTFS_WORK_INVALID_ARGUMENT,
                           ROOTFS_WORK_STAGE_ARGUMENTS, 0,
                           "paths and a 1..%u-byte I/O buffer are required",
                           ROOTFS_WORK_MAX_IO_BUFFER);
    }
    result->io_buffer_bytes = selected.io_buffer_bytes;
    buffer = (uint8_t *)malloc(selected.io_buffer_bytes);
    if (!buffer)
        return result_fail(result, ROOTFS_WORK_NO_MEMORY,
                           ROOTFS_WORK_STAGE_ARGUMENTS, 0,
                           "cannot allocate %zu-byte bounded I/O buffer",
                           selected.io_buffer_bytes);

#ifdef _WIN32
    if (!windows_open_source(source_path, &source, &source_before, result))
        goto done;
    if (!windows_split_destination(destination_path, &destination, result))
        goto done;
#else
    if (!posix_open_source(source_path, &source, &source_before, result))
        goto done;
    if (!posix_open_destination(destination_path, &destination, result))
        goto done;
#endif
    result->source_size = source_before.size;
    if (source_before.size > IOS3_SHA256_MAX_INPUT_BYTES) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR,
                    ROOTFS_WORK_STAGE_SOURCE_IDENTITY, 0,
                    "source is too large for a bounded SHA-256 bit count");
        goto done;
    }
    if (selected.source_identity.required &&
        selected.source_identity.expected_size != source_before.size) {
        result_fail(result, ROOTFS_WORK_SOURCE_IDENTITY_MISMATCH,
                    ROOTFS_WORK_STAGE_SOURCE_IDENTITY, 0,
                    "source size does not match the required identity");
        goto done;
    }
    if (!hfs_validate(&source, source_before.size, &source_volume, buffer,
                      selected.io_buffer_bytes,
                      ROOTFS_WORK_STAGE_SOURCE_VALIDATE, result))
        goto done;
    if (!destination_temp_create(&destination, &temporary,
                                 &temporary_created, result))
        goto done;
    if (!host_file_resize(&temporary, source_before.size, &error)) {
        result_fail(result, ROOTFS_WORK_WRITE_FAILED,
                    ROOTFS_WORK_STAGE_COPY, error,
                    "cannot size temporary image for source copy");
        goto done;
    }
    if (!ios3_sha256_init(&source_sha256)) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR,
                    ROOTFS_WORK_STAGE_SOURCE_IDENTITY, 0,
                    "cannot initialize source SHA-256 state");
        goto done;
    }
    if (!copy_source(&source, &temporary, source_before.size, buffer,
                     selected.io_buffer_bytes, &source_sha256, result))
        goto done;
    if (!host_file_stamp(&source, &source_after, &error)) {
        result_fail(result, ROOTFS_WORK_SOURCE_CHANGED,
                    ROOTFS_WORK_STAGE_COPY_VERIFY, error,
                    "cannot revalidate source identity after copy");
        goto done;
    }
    if (!stamp_equal(&source_before, &source_after)) {
        result_fail(result, ROOTFS_WORK_SOURCE_CHANGED,
                    ROOTFS_WORK_STAGE_COPY_VERIFY, 0,
                    "source identity, size, links, or timestamps changed during copy");
        goto done;
    }
    if (!ios3_sha256_final(&source_sha256, result->source_sha256)) {
        result_fail(result, ROOTFS_WORK_RANGE_ERROR,
                    ROOTFS_WORK_STAGE_SOURCE_IDENTITY, 0,
                    "cannot finalize the bounded source SHA-256 digest");
        goto done;
    }
    result->source_sha256_valid = true;
    if (selected.source_identity.required) {
        if (memcmp(result->source_sha256,
                   selected.source_identity.expected_sha256,
                   IOS3_SHA256_DIGEST_SIZE) != 0) {
            result_fail(result, ROOTFS_WORK_SOURCE_IDENTITY_MISMATCH,
                        ROOTFS_WORK_STAGE_SOURCE_IDENTITY, 0,
                        "source SHA-256 does not match the required identity");
            goto done;
        }
        result->source_identity_verified = true;
    }
    if (!host_file_stamp(&temporary, &temporary_stamp, &error) ||
        temporary_stamp.size != source_before.size) {
        result_fail(result, ROOTFS_WORK_SOURCE_CHANGED,
                    ROOTFS_WORK_STAGE_COPY_VERIFY, error,
                    "temporary copy size does not match the immutable source");
        goto done;
    }
    if (!hfs_validate(&temporary, source_before.size, &copied_volume, buffer,
                      selected.io_buffer_bytes,
                      ROOTFS_WORK_STAGE_COPY_VERIFY, result))
        goto done;
    if (memcmp(&source_volume, &copied_volume, sizeof(source_volume)) != 0) {
        result_fail(result, ROOTFS_WORK_SOURCE_CHANGED,
                    ROOTFS_WORK_STAGE_COPY_VERIFY, 0,
                    "copied HFS metadata does not match source metadata");
        goto done;
    }
    if (!host_file_close(&source, &error)) {
        result_fail(result, ROOTFS_WORK_SOURCE_CHANGED,
                    ROOTFS_WORK_STAGE_COPY_VERIFY, error,
                    "source close failed after immutable copy");
        goto done;
    }
    work_size = source_before.size;
    if (!fstab_rewrite(&temporary, work_size, selected.fstab_line, buffer,
                       selected.io_buffer_bytes, result))
        goto done;
    if (!grow_volume(&temporary, &work_size, selected.growth_bytes,
                     &copied_volume, buffer, selected.io_buffer_bytes, result))
        goto done;
    if (!hfs_validate(&temporary, work_size, &final_volume, buffer,
                      selected.io_buffer_bytes,
                      ROOTFS_WORK_STAGE_FINAL_VALIDATE, result))
        goto done;
    if (selected.growth_bytes != 0u &&
        final_volume.total_blocks <= copied_volume.total_blocks) {
        result_fail(result, ROOTFS_WORK_HFS_INVALID,
                    ROOTFS_WORK_STAGE_FINAL_VALIDATE, 0,
                    "final validation did not observe planned volume growth");
        goto done;
    }
    result->final_size = work_size;
    if (!host_file_sync(&temporary, &error)) {
        result_fail(result, ROOTFS_WORK_SYNC_FAILED,
                    ROOTFS_WORK_STAGE_FLUSH, error,
                    "cannot flush completed temporary work image");
        goto done;
    }
    if (!host_file_close(&temporary, &error)) {
        result_fail(result, ROOTFS_WORK_SYNC_FAILED,
                    ROOTFS_WORK_STAGE_FLUSH, error,
                    "cannot close flushed temporary work image");
        goto done;
    }
    if (!publish(&destination, result))
        goto done;
    result->status = ROOTFS_WORK_OK;
    result->stage = ROOTFS_WORK_STAGE_NONE;
    result->system_error = 0;
    (void)snprintf(result->detail, sizeof(result->detail),
                   "published %" PRIu64 "-byte validated rootfs work image",
                   work_size);
    success = true;

done:
    if (host_file_is_open(&source) &&
        !host_file_close(&source, &error) &&
        result->cleanup_system_error == 0)
        result->cleanup_system_error = error;
    if (host_file_is_open(&temporary) &&
        !host_file_close(&temporary, &error)) {
        if (result->cleanup_system_error == 0)
            result->cleanup_system_error = error;
        if (temporary_created)
            result->temporary_left = true;
    }
    if (!success)
        cleanup_unpublished(&destination, temporary_created, result);
    destination_release(&destination);
    free(buffer);
    return result->status;
}

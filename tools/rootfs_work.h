/*
 * iOS3-VM -- bounded host-side rootfs work-image provisioning.
 *
 * This interface deliberately owns no guest or emulator state.  It copies an
 * immutable bare HFS+/HFSX source into a new file beside the requested
 * destination name, applies the narrowly-defined fstab and volume-growth
 * transformations to that unpublished temporary file, validates the result,
 * flushes it, and publishes it without replacing an existing path.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_ROOTFS_WORK_H
#define IOS3VM_ROOTFS_WORK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ROOTFS_WORK_MAX_IO_BUFFER (1024u * 1024u)
#define ROOTFS_WORK_DETAIL_CAPACITY 256u

#define ROOTFS_WORK_DEFAULT_FSTAB "/dev/md0 / hfs rw,update 0 1"

typedef enum rootfs_work_status {
    ROOTFS_WORK_OK = 0,
    ROOTFS_WORK_INVALID_ARGUMENT,
    ROOTFS_WORK_NO_MEMORY,
    ROOTFS_WORK_PATH_UNSAFE,
    ROOTFS_WORK_SOURCE_OPEN_FAILED,
    ROOTFS_WORK_SOURCE_NOT_REGULAR,
    ROOTFS_WORK_SOURCE_ALIAS,
    ROOTFS_WORK_SOURCE_BUSY,
    ROOTFS_WORK_DESTINATION_EXISTS,
    ROOTFS_WORK_DESTINATION_OPEN_FAILED,
    ROOTFS_WORK_TEMP_CREATE_FAILED,
    ROOTFS_WORK_READ_FAILED,
    ROOTFS_WORK_WRITE_FAILED,
    ROOTFS_WORK_SYNC_FAILED,
    ROOTFS_WORK_SOURCE_CHANGED,
    ROOTFS_WORK_SOURCE_IDENTITY_MISMATCH,
    ROOTFS_WORK_HFS_INVALID,
    ROOTFS_WORK_FSTAB_NOT_UNIQUE,
    ROOTFS_WORK_FSTAB_LINE_INVALID,
    ROOTFS_WORK_GROW_INVALID,
    ROOTFS_WORK_RANGE_ERROR,
    ROOTFS_WORK_PUBLISH_FAILED,
    ROOTFS_WORK_PUBLISH_DURABILITY_FAILED
} rootfs_work_status_t;

typedef enum rootfs_work_stage {
    ROOTFS_WORK_STAGE_NONE = 0,
    ROOTFS_WORK_STAGE_ARGUMENTS,
    ROOTFS_WORK_STAGE_SOURCE_PATH,
    ROOTFS_WORK_STAGE_DESTINATION_PATH,
    ROOTFS_WORK_STAGE_SOURCE_OPEN,
    ROOTFS_WORK_STAGE_SOURCE_VALIDATE,
    ROOTFS_WORK_STAGE_SOURCE_IDENTITY,
    ROOTFS_WORK_STAGE_TEMP_CREATE,
    ROOTFS_WORK_STAGE_COPY,
    ROOTFS_WORK_STAGE_COPY_VERIFY,
    ROOTFS_WORK_STAGE_FSTAB_SCAN,
    ROOTFS_WORK_STAGE_FSTAB_WRITE,
    ROOTFS_WORK_STAGE_GROW_PLAN,
    ROOTFS_WORK_STAGE_GROW_WRITE,
    ROOTFS_WORK_STAGE_FINAL_VALIDATE,
    ROOTFS_WORK_STAGE_FLUSH,
    ROOTFS_WORK_STAGE_PUBLISH,
    ROOTFS_WORK_STAGE_DIRECTORY_SYNC,
    ROOTFS_WORK_STAGE_CLEANUP
} rootfs_work_stage_t;

typedef struct rootfs_work_source_identity {
    bool required;
    uint64_t expected_size;
    uint8_t expected_sha256[IOS3_SHA256_DIGEST_SIZE];
} rootfs_work_source_identity_t;

typedef struct rootfs_work_options {
    /* NULL selects ROOTFS_WORK_DEFAULT_FSTAB. */
    const char *fstab_line;

    /*
     * Same arithmetic as bootkernel's historical --grow implementation:
     * floor(growth_bytes / allocationBlockSize), less the old reserved-tail
     * block, is added to totalBlocks and then clamped to the existing
     * allocation bitmap's bit capacity.  Zero disables growth.
     *
     * For compatibility with the already-proven bootkernel transformation,
     * this narrow image builder leaves HFS lastMountedVersion and writeCount
     * unchanged. It is not a general-purpose HFS writer or fsck replacement.
     */
    uint64_t growth_bytes;

    /* Zero selects ROOTFS_WORK_MAX_IO_BUFFER; otherwise 1..that limit. */
    size_t io_buffer_bytes;

    /*
     * Optional exact source gate.  When required is true, expected_size is
     * checked before a temporary file is created and expected_sha256 is
     * checked after the immutable source has been copied and re-stamped, but
     * before any HFS transformation or publication.  The fields are ignored
     * when required is false; the observed digest is still reported.
     */
    rootfs_work_source_identity_t source_identity;
} rootfs_work_options_t;

typedef struct rootfs_work_result {
    rootfs_work_status_t status;
    rootfs_work_stage_t stage;
    int system_error;
    int cleanup_system_error;
    uint64_t source_size;
    uint64_t final_size;
    uint64_t bytes_copied;
    uint64_t fstab_offset;
    uint8_t source_sha256[IOS3_SHA256_DIGEST_SIZE];
    size_t io_buffer_bytes;
    bool source_sha256_valid;
    bool source_identity_verified;
    bool published;
    bool temporary_left;
    char detail[ROOTFS_WORK_DETAIL_CAPACITY];
} rootfs_work_result_t;

/*
 * Create destination_path.  The destination must not exist.  On success the
 * exact published size is in result->final_size.  Once result->published is
 * true, the complete destination is intentionally preserved even if a later
 * durability or temporary-link cleanup step returns non-OK; callers must
 * inspect published on every failure.
 *
 * This remains a generic format transformer, not a signature verifier.  Its
 * optional source_identity policy provides an exact caller-selected size and
 * SHA-256 gate without a second source read.  Both backends reject link/reparse
 * ambiguity and publish without replacement, but portable POSIX linkat and
 * Win32 MoveFileEx still name the temporary file by path.  Atomic destination-
 * entry creation is guaranteed; object identity is not a security boundary
 * against a hostile same-user namespace racer.
 */
rootfs_work_status_t rootfs_work_create(const char *source_path,
                                        const char *destination_path,
                                        const rootfs_work_options_t *options,
                                        rootfs_work_result_t *result);

const char *rootfs_work_status_name(rootfs_work_status_t status);
const char *rootfs_work_stage_name(rootfs_work_stage_t stage);

#ifdef __cplusplus
}
#endif

#endif /* IOS3VM_ROOTFS_WORK_H */

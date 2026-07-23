/*
 * iOS3-VM -- bounded host-backed XNU32 raw memory-disk bridge.
 *
 * A frontend may replace one audited Thumb memory-disk character entry with
 * `svc; bx lr` and install md_raw_bridge_handle_svc() on the ARM bus.  The
 * portable core owns no firmware address or patch byte: the exact site,
 * encoding, device number, RAM aperture, and media session are configuration.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_MD_RAW_BRIDGE_H
#define IOS3VM_MD_RAW_BRIDGE_H

#include "arm.h"
#include "vm_block.h"

#include <stdbool.h>
#include <stdint.h>

#define MD_RAW_BRIDGE_PAGE_SIZE UINT32_C(4096)
#define MD_RAW_BRIDGE_PERMISSION_GRANULE UINT32_C(1024)
#define MD_RAW_BRIDGE_MAX_TRANSFER UINT32_C(131072)
#define MD_RAW_BRIDGE_MAX_IOVECS UINT32_C(1024)
#define MD_RAW_BRIDGE_MAX_DATA_SPANS \
    (MD_RAW_BRIDGE_MAX_IOVECS * UINT32_C(2) + \
     MD_RAW_BRIDGE_MAX_TRANSFER / MD_RAW_BRIDGE_PERMISSION_GRANULE)

/* Exact 32-bit XNU uio layout implemented by this bridge. */
#define MD_RAW_BRIDGE_UIO_SIZE UINT32_C(0x28)
#define MD_RAW_BRIDGE_USER_IOV_SIZE UINT32_C(8)

typedef enum {
    MD_RAW_BRIDGE_DIRECTION_NONE = 0,
    MD_RAW_BRIDGE_DIRECTION_READ,
    MD_RAW_BRIDGE_DIRECTION_WRITE
} md_raw_bridge_direction_t;

typedef struct {
    /* First halfword of the replaced four-byte Thumb function prologue. */
    uint32_t pc;
    /* Exact zero-extended Thumb SVC halfword installed at pc. */
    uint32_t encoding;
} md_raw_bridge_site_t;

typedef struct {
    md_raw_bridge_site_t site;
    /* Complete Darwin dev_t accepted by this backend (for 7E18 md0: 09000000). */
    uint32_t expected_device;
    /* Exclusive user VA ceiling enforced before unprivileged translation. */
    uint32_t user_address_limit;
    uint64_t media_size;
    uint64_t ram_base;
    uint64_t ram_size;
    uint8_t *ram;
    const vm_block_t *block;
    vm_block_cancel_fn cancelled;
    void *cancel_context;
} md_raw_bridge_config_t;

typedef struct {
    uint64_t successful_reads;
    uint64_t successful_writes;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint64_t zero_length_requests;
    uint64_t guest_errors;
    uint64_t failures;
} md_raw_bridge_stats_t;

/* Stable reasons for either a returned guest errno or a fail-closed halt. */
typedef enum {
    MD_RAW_BRIDGE_ERROR_NONE = 0,
    MD_RAW_BRIDGE_ERROR_NULL_CPU,
    MD_RAW_BRIDGE_ERROR_INVALID_MODE,
    MD_RAW_BRIDGE_ERROR_INVALID_CONFIG,
    MD_RAW_BRIDGE_ERROR_MMU_DISABLED,
    MD_RAW_BRIDGE_ERROR_MISSING_BUS_ACCESS,
    MD_RAW_BRIDGE_ERROR_DEVICE,
    MD_RAW_BRIDGE_ERROR_UIO_ALIGNMENT,
    MD_RAW_BRIDGE_ERROR_UIO_TRANSLATION,
    MD_RAW_BRIDGE_ERROR_UIO_RANGE,
    MD_RAW_BRIDGE_ERROR_UIO_STATE,
    MD_RAW_BRIDGE_ERROR_SEGMENT,
    MD_RAW_BRIDGE_ERROR_DIRECTION,
    MD_RAW_BRIDGE_ERROR_RESIDUAL,
    MD_RAW_BRIDGE_ERROR_OFFSET,
    MD_RAW_BRIDGE_ERROR_MEDIA_RANGE,
    MD_RAW_BRIDGE_ERROR_IOV_COUNT,
    MD_RAW_BRIDGE_ERROR_IOV_ALIGNMENT,
    MD_RAW_BRIDGE_ERROR_IOV_TRANSLATION,
    MD_RAW_BRIDGE_ERROR_IOV_RANGE,
    MD_RAW_BRIDGE_ERROR_IOV_SUM,
    MD_RAW_BRIDGE_ERROR_USER_ADDRESS,
    MD_RAW_BRIDGE_ERROR_USER_TRANSLATION,
    MD_RAW_BRIDGE_ERROR_USER_RANGE,
    MD_RAW_BRIDGE_ERROR_METADATA_ALIAS,
    MD_RAW_BRIDGE_ERROR_PLAN_CAPACITY,
    MD_RAW_BRIDGE_ERROR_BLOCK_IO
} md_raw_bridge_error_code_t;

typedef struct {
    md_raw_bridge_error_code_t code;
    md_raw_bridge_direction_t direction;
    uint32_t pc;
    uint32_t encoding;
    uint32_t device;
    uint32_t uio_va;
    uint32_t iov_va;
    uint32_t fault_va;
    uint32_t fault_pa;
    uint32_t mmu_status;
    uint32_t segment;
    uint32_t rw;
    int32_t iov_count;
    int32_t residual;
    int guest_errno;
    uint64_t media_offset;
    uint64_t transferred;
    vm_block_status_t block_status;
} md_raw_bridge_error_t;

typedef struct {
    uint32_t base_pa;
    uint32_t length_pa;
    uint32_t base;
    uint32_t length;
    /* Prefix consumed by this request; may be shorter than length. */
    uint32_t consumed;
} md_raw_bridge_iov_plan_t;

typedef struct {
    uint32_t pa;
    uint32_t length;
} md_raw_bridge_data_span_t;

/*
 * No allocation or host FILE state enters emucore. The copied config borrows
 * RAM, vm_block, callback, and callback-context lifetimes from its owner. One
 * bridge is single-CPU and non-reentrant: its bounded staging buffer and plans
 * are reused per call. The object is roughly 170 KiB; place it in static or
 * heap storage, never on a small iOS thread stack.
 */
typedef struct {
    md_raw_bridge_config_t config;
    md_raw_bridge_stats_t stats;
    /* Success does not erase the most recent diagnostic. */
    md_raw_bridge_error_t last_error;
    /* A later fatal bridge error does not erase guest-errno attribution. */
    md_raw_bridge_error_t last_guest_error;
    uint32_t iov_plan_count;
    uint32_t data_span_count;
    md_raw_bridge_iov_plan_t iov_plan[MD_RAW_BRIDGE_MAX_IOVECS];
    md_raw_bridge_data_span_t data_spans[MD_RAW_BRIDGE_MAX_DATA_SPANS];
    uint8_t scratch[MD_RAW_BRIDGE_MAX_TRANSFER];
} md_raw_bridge_t;

bool md_raw_bridge_config_valid(const md_raw_bridge_config_t *config);
void md_raw_bridge_init(md_raw_bridge_t *bridge,
                        const md_raw_bridge_config_t *config);

/*
 * Suitable as an arm_privileged_svc_handler_t.  A recognized call changes only
 * r0 (Darwin errno); the patched BX LR performs the ordinary function return.
 * Guest uio/iovec memory is committed only after a complete backend transfer.
 */
arm_svc_result_t md_raw_bridge_handle_svc(void *context, arm_cpu_t *cpu,
                                          uint32_t pc, uint32_t encoding);

const char *md_raw_bridge_error_string(md_raw_bridge_error_code_t code);

#endif /* IOS3VM_MD_RAW_BRIDGE_H */

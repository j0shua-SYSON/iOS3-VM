/*
 * iOS3-VM — persistent storage backing.
 *
 * Why this exists: a jailbreak is fundamentally a *persistent* modification to
 * the guest's storage. Installing Cydia, remounting / read-write, keeping your
 * changes, and above all demonstrating an *untethered* jailbreak that survives
 * a reboot — none of that is possible if guest writes evaporate when the
 * emulator exits. Booting from a RAM disk alone would wipe the jailbreak every
 * launch.
 *
 * This module saves and restores device contents to a host file. It gives full
 * persistence WITHOUT requiring Apple's proprietary FTL on-flash format: the
 * bytes are ours to store however we like, and the guest sees a device whose
 * state survives. Apple's real VFL/FTL remains future work for authentic
 * jailbreak-tool research (see nand.h).
 *
 * The container carries the geometry it was written with, so restoring into a
 * differently-shaped device is refused rather than silently misinterpreted.
 *
 * Only stdio is used, so the core stays portable; the caller supplies the path
 * and therefore owns all platform policy (on iOS, the app's Documents dir).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_STORAGE_H
#define IOS3VM_STORAGE_H

#include <stdbool.h>
#include <stdint.h>
#include "nand.h"
#include "soc.h"

typedef enum {
    STORAGE_OK = 0,
    STORAGE_ERR_IO,           /* could not open/read/write the file        */
    STORAGE_ERR_FORMAT,       /* not one of our images                     */
    STORAGE_ERR_VERSION,      /* written by an incompatible version        */
    STORAGE_ERR_GEOMETRY,     /* geometry does not match the target device */
    STORAGE_ERR_TRUNCATED     /* file shorter than its header promises     */
} storage_status_t;

/* Write the NAND's contents (data, spare and bad-block map) to `path`. */
storage_status_t storage_save_nand(const nand_t *n, const char *path);

/*
 * Restore into an already-initialised NAND. The device's geometry must match
 * the one recorded in the file, otherwise STORAGE_ERR_GEOMETRY is returned and
 * the device is left untouched.
 */
storage_status_t storage_load_nand(nand_t *n, const char *path);

/*
 * NOR persistence. An untethered jailbreak on this SoC (24kpwn) persists its
 * payload in NOR, so flash changes must survive a relaunch just as NAND
 * changes do.
 */
storage_status_t storage_save_nor(const s5l_nor_t *n, const char *path);
storage_status_t storage_load_nor(s5l_nor_t *n, const char *path);

const char *storage_strerror(storage_status_t st);

#endif /* IOS3VM_STORAGE_H */

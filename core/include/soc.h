/*
 * iOS3-VM — Samsung S5L8900 system-on-chip model.
 *
 * The S5L8900 is the application processor in the original iPhone, the iPhone
 * 3G, and the iPod touch 1G — the silicon iPhone OS 1–3 ran on. This header
 * declares its memory map and the device models the CPU talks to over the bus.
 *
 * The peripheral base addresses below come from public reverse-engineering of
 * the S5L8900. They are exercised by our own bare-metal payloads today and will
 * be validated against real Apple firmware at M3.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_SOC_H
#define IOS3VM_SOC_H

#include <stddef.h>
#include <stdint.h>
#include "arm.h"

/* ------------------------------------------------------------ memory map */
#define S5L8900_SRAM_BASE   0x22000000u
#define S5L8900_SDRAM_BASE  0x08000000u
#define S5L8900_UART0_BASE  0x3cc00000u
#define S5L8900_VIC0_BASE   0x38e00000u
#define S5L8900_TIMER_BASE  0x3e200000u
#define S5L8900_CLOCK_BASE  0x3c500000u
#define S5L8900_GPIO_BASE   0x3cf00000u
#define S5L8900_DEV_SIZE    0x00001000u   /* per-peripheral window */

/* --------------------------------------------------------------- UART ---
 * Samsung-style UART (as on S3C-family parts). Writing a byte to UTXH
 * transmits it; UTRSTAT reports the transmitter permanently ready, so guest
 * "wait until TX empty" spin loops make progress immediately.
 */
#define UART_ULCON   0x00u
#define UART_UCON    0x04u
#define UART_UFCON   0x08u
#define UART_UMCON   0x0cu
#define UART_UTRSTAT 0x10u
#define UART_UERSTAT 0x14u
#define UART_UFSTAT  0x18u
#define UART_UMSTAT  0x1cu
#define UART_UTXH    0x20u
#define UART_URXH    0x24u
#define UART_UBRDIV  0x28u

#define UART_TX_BUFFER 8192

typedef struct {
    uint32_t ulcon, ucon, ufcon, umcon, ubrdiv;
    char     tx[UART_TX_BUFFER];   /* everything the guest has printed */
    size_t   tx_len;
} s5l_uart_t;

void     s5l_uart_reset(s5l_uart_t *u);
uint32_t s5l_uart_read(s5l_uart_t *u, uint32_t off);
void     s5l_uart_write(s5l_uart_t *u, uint32_t off, uint32_t val);

/* ------------------------------------------------------------- machine ---
 * Wires the CPU to RAM and the peripherals through one arm_bus_t.
 */
typedef struct {
    arm_cpu_t  cpu;
    arm_bus_t  bus;
    uint8_t   *ram;
    uint32_t   ram_base;
    uint32_t   ram_size;
    s5l_uart_t uart0;
    uint64_t   unmapped_reads;   /* visibility: accesses outside the map */
    uint64_t   unmapped_writes;
} s5l8900_t;

/* ram_base/ram_size define where RAM appears. Returns false on allocation
 * failure. Call s5l8900_free() when done. */
bool s5l8900_init(s5l8900_t *m, uint32_t ram_base, uint32_t ram_size);
void s5l8900_free(s5l8900_t *m);

/* Copy a blob into guest RAM at a physical address. */
void s5l8900_load(s5l8900_t *m, uint32_t addr, const void *data, size_t len);

/* Run up to max_steps instructions, stopping early on a non-OK status.
 * Returns the number of instructions retired. */
unsigned s5l8900_run(s5l8900_t *m, unsigned max_steps, arm_status_t *status);

#endif /* IOS3VM_SOC_H */

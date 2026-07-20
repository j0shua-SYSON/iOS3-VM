/*
 * iOS3-VM — S5L8900 UART.
 *
 * This is the first device that makes the emulator *observable*: whatever the
 * guest writes to UTXH is captured, which is how iBoot and the XNU kernel will
 * later tell us how far they got.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <string.h>

/* UTRSTAT bits: [0] receive data ready, [1] TX buffer empty, [2] transmitter
 * empty. We report the transmitter permanently drained so guest spin-loops of
 * the form "wait until TX empty, then write" always make progress. */
#define UTRSTAT_TX_EMPTY     (1u << 2)
#define UTRSTAT_TX_BUF_EMPTY (1u << 1)

void s5l_uart_reset(s5l_uart_t *u) {
    memset(u, 0, sizeof *u);
}

uint32_t s5l_uart_read(s5l_uart_t *u, uint32_t off) {
    switch (off) {
        case UART_ULCON:   return u->ulcon;
        case UART_UCON:    return u->ucon;
        case UART_UFCON:   return u->ufcon;
        case UART_UMCON:   return u->umcon;
        case UART_UBRDIV:  return u->ubrdiv;
        case UART_UTRSTAT: return UTRSTAT_TX_EMPTY | UTRSTAT_TX_BUF_EMPTY;
        case UART_UERSTAT: return 0;
        case UART_UFSTAT:  return 0;   /* FIFOs always empty */
        case UART_UMSTAT:  return 0;
        case UART_URXH:    return 0;   /* no input source yet */
        default:           return 0;
    }
}

void s5l_uart_write(s5l_uart_t *u, uint32_t off, uint32_t val) {
    switch (off) {
        case UART_ULCON:  u->ulcon  = val; break;
        case UART_UCON:   u->ucon   = val; break;
        case UART_UFCON:  u->ufcon  = val; break;
        case UART_UMCON:  u->umcon  = val; break;
        case UART_UBRDIV: u->ubrdiv = val; break;
        case UART_UTXH:
            if (u->tx_len < UART_TX_BUFFER - 1) u->tx[u->tx_len++] = (char)(val & 0xff);
            break;
        default: break;
    }
}

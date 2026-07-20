/*
 * iOS3-VM — S5L8900 periodic timer.
 *
 * A down-counter that reloads and raises an interrupt at zero. Timer interrupts
 * are what drive an OS scheduler, so this plus the VIC completes the path
 * "device -> controller -> CPU exception" end to end.
 *
 * NOTE: the register layout here is provisional (see soc.h) — sufficient for
 * our own payloads, to be replaced by the real timer block at M3.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <string.h>

void s5l_timer_reset(s5l_timer_t *t) { memset(t, 0, sizeof *t); }

uint32_t s5l_timer_read(s5l_timer_t *t, uint32_t off) {
    switch (off) {
        case TIMER_CTRL:    return t->ctrl;
        case TIMER_RELOAD:  return t->reload;
        case TIMER_VALUE:   return t->value;
        case TIMER_INTSTAT: return t->intstat;
        default:            return 0;
    }
}

void s5l_timer_write(s5l_timer_t *t, uint32_t off, uint32_t val) {
    switch (off) {
        case TIMER_CTRL:
            t->ctrl = val;
            if (val & TIMER_CTRL_ENABLE) t->value = t->reload;  /* (re)start */
            break;
        case TIMER_RELOAD:  t->reload = val; t->value = val; break;
        case TIMER_VALUE:   t->value = val; break;
        case TIMER_INTSTAT: t->intstat = 0; break;              /* write clears */
        default: break;
    }
}

bool s5l_timer_tick(s5l_timer_t *t, uint32_t ticks) {
    if (!(t->ctrl & TIMER_CTRL_ENABLE)) return t->intstat != 0;

    while (ticks--) {
        if (t->value == 0) t->value = t->reload;   /* begin a period */
        if (t->value == 0) break;                  /* reload 0: nothing to count */

        /* Exactly one expiry per reload period. Latching on both the
         * decrement-to-zero and the reload-from-zero paths would raise two
         * interrupts per period, at intervals N, 1, N, 1, ... */
        if (--t->value == 0) {
            if (t->ctrl & TIMER_CTRL_INT_EN) t->intstat = 1;
            t->value = t->reload;                  /* reload immediately */
        }
    }
    return t->intstat != 0;
}

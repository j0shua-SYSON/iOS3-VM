/*
 * iOS3-VM — S5L8900 vectored interrupt controller (PL190-style).
 *
 * Devices assert numbered lines; the controller masks them by the enable
 * register and routes each to IRQ or FIQ. Its outputs drive arm_cpu_t's
 * irq_line/fiq_line, which arm_step samples before every fetch.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "soc.h"
#include <string.h>

static inline uint32_t pending(const s5l_vic_t *v) { return v->raw | v->soft; }

void s5l_vic_reset(s5l_vic_t *v) { memset(v, 0, sizeof *v); }

bool s5l_vic_irq(const s5l_vic_t *v) {
    return (pending(v) & v->enable & ~v->select) != 0;
}
bool s5l_vic_fiq(const s5l_vic_t *v) {
    return (pending(v) & v->enable & v->select) != 0;
}

void s5l_vic_set_line(s5l_vic_t *v, unsigned line, bool level) {
    if (line > 31) return;
    if (level) v->raw |= (1u << line);
    else       v->raw &= ~(1u << line);
}

uint32_t s5l_vic_read(s5l_vic_t *v, uint32_t off) {
    switch (off) {
        case VIC_IRQSTATUS: return pending(v) & v->enable & ~v->select;
        case VIC_FIQSTATUS: return pending(v) & v->enable & v->select;
        case VIC_RAWINTR:   return pending(v);
        case VIC_INTSELECT: return v->select;
        case VIC_INTENABLE: return v->enable;
        case VIC_SOFTINT:   return v->soft;
        default:            return 0;
    }
}

void s5l_vic_write(s5l_vic_t *v, uint32_t off, uint32_t val) {
    switch (off) {
        case VIC_INTSELECT:    v->select  = val;   break;
        case VIC_INTENABLE:    v->enable |= val;   break;  /* write 1 to set   */
        case VIC_INTENCLEAR:   v->enable &= ~val;  break;  /* write 1 to clear */
        case VIC_SOFTINT:      v->soft   |= val;   break;
        case VIC_SOFTINTCLEAR: v->soft   &= ~val;  break;
        default: break;
    }
}

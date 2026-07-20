/*
 * iOS3-VM — the on-device demo guest. See VMGuest.h for what this is for.
 *
 * The payload is emitted here by a ~40-line assembler rather than written out
 * as a table of magic words. That is not decoration: every branch target is
 * then computed from the label's actual position, so inserting an instruction
 * cannot silently break a loop, and each encoding appears exactly once next to
 * the mnemonic it implements.
 *
 * The program, in ARM assembly:
 *
 *     ldr   r7, =UART0_BASE
 *     ldr   r8, =FB_BASE
 *     mov   r9, #320                 @ width
 *     mov   r10,#480                 @ height
 *     mov   r4, #0                   @ t: frame counter
 *     ldr   r6, =banner
 * 1:  ldrb  r5, [r6], #1             @ print(banner)
 *     cmp   r5, #0
 *     strne r5, [r7, #UART_UTXH]
 *     bne   1b
 * frame:
 *     mov   r0, r8                   @ dst = framebuffer
 *     mov   r3, #0                   @ y = 0
 * yloop:
 *     add   r11, r3, r4              @ green and alpha are constant per row,
 *     and   r11, r11, #0xff          @ so hoist them out of the inner loop
 *     mov   r11, r11, lsl #8
 *     orr   r11, r11, #0xff000000
 *     mov   r2, #0                   @ x = 0
 * xloop:
 *     eor   r5, r2, r3               @ blue  = (x ^ y) & 0xff
 *     and   r5, r5, #0xff
 *     add   r6, r2, r4               @ red   = (x + t) & 0xff
 *     and   r6, r6, #0xff
 *     orr   r5, r5, r6, lsl #16
 *     orr   r5, r5, r11              @ green = (y + t) & 0xff, alpha = 0xff
 *     str   r5, [r0], #4
 *     add   r2, r2, #1
 *     cmp   r2, r9
 *     bne   xloop
 *     add   r3, r3, #1
 *     cmp   r3, r10
 *     bne   yloop
 *     add   r4, r4, #1
 *     ldr   r6, =msg                 @ print("frame ")
 * 2:  ldrb  r5, [r6], #1
 *     cmp   r5, #0
 *     strne r5, [r7, #UART_UTXH]
 *     bne   2b
 *     mov   r5, r4, lsr #4           @ print(hex(t & 0xff))
 *     and   r5, r5, #0x0f
 *     cmp   r5, #9
 *     add   r5, r5, #'0'
 *     addhi r5, r5, #7
 *     str   r5, [r7, #UART_UTXH]
 *     and   r5, r4, #0x0f
 *     cmp   r5, #9
 *     add   r5, r5, #'0'
 *     addhi r5, r5, #7
 *     str   r5, [r7, #UART_UTXH]
 *     mov   r5, #'\n'
 *     str   r5, [r7, #UART_UTXH]
 *     b     frame
 *
 * Note the loops only ever branch BACKWARDS, and the two string printers exit
 * by falling through a conditional store rather than a forward branch. That is
 * why no relocation pass is needed: every target is already known when the
 * branch is emitted.
 *
 * Cost is 10 instructions per pixel, so a full 320x480 repaint is about 1.54
 * million instructions. On an interpreter retiring a few million instructions a
 * second that is a fraction of a second per frame — which is exactly the point:
 * the UI samples guest DRAM asynchronously, so you watch the raster sweep down
 * the panel. A finished frame appearing atomically would look like a mock.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "VMGuest.h"

#include <string.h>

/* ---------------------------------------------------------- encodings --- */

/* Condition codes (bits 31:28). */
#define C_HI 0x8u   /* unsigned higher      */
#define C_NE 0x1u   /* not equal            */
#define C_AL 0xeu   /* always               */
#define COND(c) ((uint32_t)(c) << 28)

/* Data-processing opcodes (bits 24:21). */
#define OP_AND 0x0u
#define OP_EOR 0x1u
#define OP_ADD 0x4u
#define OP_CMP 0xau
#define OP_ORR 0xcu
#define OP_MOV 0xdu

/* Barrel-shift types (bits 6:5). */
#define SH_LSL 0u
#define SH_LSR 1u

/*
 * Data processing with an immediate second operand. The operand is
 * ror(imm8, 2*rot) — ARM has no 32-bit literal field, which is why constants
 * like 320 are encoded as 0x50 rotated rather than written out.
 */
static uint32_t dp_imm(unsigned cond, unsigned op, unsigned s,
                       unsigned rn, unsigned rd, unsigned rot, unsigned imm8) {
    return COND(cond) | 0x02000000u | (op << 21) | (s << 20)
         | (rn << 16) | (rd << 12) | (rot << 8) | imm8;
}

/* Data processing with a register second operand, shifted by a constant. */
static uint32_t dp_reg(unsigned cond, unsigned op, unsigned s,
                       unsigned rn, unsigned rd, unsigned rm,
                       unsigned shtype, unsigned shamt) {
    return COND(cond) | (op << 21) | (s << 20) | (rn << 16) | (rd << 12)
         | ((shamt & 31u) << 7) | (shtype << 5) | rm;
}

/* Single data transfer with a 12-bit immediate offset. */
static uint32_t ldst(unsigned cond, bool load, bool byte, bool pre, bool up,
                     bool wb, unsigned rn, unsigned rd, unsigned imm12) {
    return COND(cond) | 0x04000000u
         | (pre  ? 1u << 24 : 0u) | (up   ? 1u << 23 : 0u)
         | (byte ? 1u << 22 : 0u) | (wb   ? 1u << 21 : 0u)
         | (load ? 1u << 20 : 0u)
         | (rn << 16) | (rd << 12) | (imm12 & 0xfffu);
}

/* ------------------------------------------------------------ emitter --- */

/* Word index of the literal pool, and byte offset of the string blob, within
 * VM_GUEST_BLOB_BYTES. Both are far enough past the code that no relocation is
 * needed; vm_guest_install() asserts the code actually fits. */
#define LIT_WORD  0x40u        /* byte offset 0x100 */
#define STR_OFF   0x140u

typedef struct { uint32_t *w; unsigned n; } emitter_t;

static void emit(emitter_t *e, uint32_t insn) { e->w[e->n++] = insn; }

/* Branch to an already-emitted label. The 24-bit field counts words from
 * pc + 8, hence the -2. */
static void emit_b(emitter_t *e, unsigned cond, unsigned target_word) {
    int32_t off = (int32_t)target_word - (int32_t)e->n - 2;
    e->w[e->n++] = COND(cond) | 0x0a000000u | ((uint32_t)off & 0x00ffffffu);
}

/* LDR Rd,[pc,#imm] — pull a 32-bit constant out of the literal pool. */
static void emit_ldr_lit(emitter_t *e, unsigned rd, unsigned lit_word) {
    unsigned imm = (lit_word - e->n) * 4u - 8u;
    e->w[e->n++] = COND(C_AL) | 0x059f0000u | (rd << 12) | (imm & 0xfffu);
}

/* Print a NUL-terminated string whose address is already in r6. Falls out of
 * the loop on the NUL byte, so there is no forward branch to fix up. */
static void emit_puts(emitter_t *e) {
    unsigned loop = e->n;
    emit(e, ldst(C_AL, true,  true,  false, true, false, 6, 5, 1));  /* ldrb r5,[r6],#1 */
    emit(e, dp_imm(C_AL, OP_CMP, 1, 5, 0, 0, 0));                    /* cmp  r5,#0      */
    emit(e, ldst(C_NE, false, false, true,  true, false, 7, 5, UART_UTXH));
    emit_b(e, C_NE, loop);
}

/* Print one hex digit from bits [shift+3:shift] of r4. */
static void emit_hex_digit(emitter_t *e, unsigned shift) {
    if (shift) emit(e, dp_reg(C_AL, OP_MOV, 0, 0, 5, 4, SH_LSR, shift));
    else       emit(e, dp_reg(C_AL, OP_MOV, 0, 0, 5, 4, SH_LSL, 0));
    emit(e, dp_imm(C_AL, OP_AND, 0, 5, 5, 0, 0x0fu));   /* and   r5,r5,#0xf  */
    emit(e, dp_imm(C_AL, OP_CMP, 1, 5, 0, 0, 9));       /* cmp   r5,#9       */
    emit(e, dp_imm(C_AL, OP_ADD, 0, 5, 5, 0, '0'));     /* add   r5,r5,#'0'  */
    emit(e, dp_imm(C_HI, OP_ADD, 0, 5, 5, 0, 7));       /* addhi r5,r5,#7    */
    emit(e, ldst(C_AL, false, false, true, true, false, 7, 5, UART_UTXH));
}

/* ------------------------------------------------------------- public --- */

uint32_t vm_guest_fb_pa(uint32_t ram_base, uint32_t ram_size) {
    /* Leave room for the payload itself at the bottom of DRAM, and for a
     * comfortable gap, before claiming the top for the framebuffer. */
    if (ram_size < VM_FB_BYTES + VM_GUEST_BLOB_BYTES + 0x10000u) return 0;
    return (uint32_t)((ram_base + ram_size - VM_FB_BYTES) & ~0xfffu);
}

const uint8_t *vm_guest_framebuffer(const s5l8900_t *m) {
    if (!m || !m->ram) return NULL;
    uint32_t pa = vm_guest_fb_pa(m->ram_base, m->ram_size);
    if (!pa) return NULL;
    return m->ram + (pa - m->ram_base);
}

const uint8_t *vm_guest_display(const s5l8900_t *m,
                                uint32_t *width, uint32_t *height,
                                uint32_t *stride, vm_pixel_order_t *order) {
    if (!m || !m->ram) return NULL;

#ifdef S5L8900_CLCD_BASE
    /*
     * Ask the display controller. s5l_clcd_window() returns what the guest
     * programmed into window 0 — the same registers real iBoot leaves set and
     * IOMobileFramebuffer adopts. Everything is validated before it is trusted:
     * a framebuffer the model reports outside DRAM, or larger than the buffer
     * the app allocated, is refused rather than turned into an out-of-bounds
     * read on the phone.
     */
    uint32_t fb_phys = 0, w = 0, h = 0, st = 0, fmt = 0, ord = 0;
    if (s5l_clcd_window(&m->clcd, 0, &fb_phys, &w, &h, &st, &fmt, &ord)
        && fmt == CLCD_FMT_32BPP
        && w == VM_FB_WIDTH && h == VM_FB_HEIGHT
        && st >= w * VM_FB_BPP
        && fb_phys >= m->ram_base
        && (uint64_t)(fb_phys - m->ram_base) + (uint64_t)st * h <= m->ram_size
        && (uint64_t)st * h <= VM_FB_BYTES) {
        if (width)  *width  = w;
        if (height) *height = h;
        if (stride) *stride = st;
        if (order)  *order  = (ord == CLCD_ORDER_ARGB) ? VM_ORDER_ARGB
                                                       : VM_ORDER_BGRA;
        return m->ram + (fb_phys - m->ram_base);
    }
#endif

    /* No CLCD, or window 0 not usable: the fixed framebuffer this demo paints,
     * which is 32bpp BGRA by construction (see the emitter's pixel packing). */
    uint32_t pa = vm_guest_fb_pa(m->ram_base, m->ram_size);
    if (!pa) return NULL;
    if (width)  *width  = VM_FB_WIDTH;
    if (height) *height = VM_FB_HEIGHT;
    if (stride) *stride = VM_FB_WIDTH * VM_FB_BPP;
    if (order)  *order  = VM_ORDER_BGRA;
    return m->ram + (pa - m->ram_base);
}

bool vm_guest_install(s5l8900_t *m) {
    if (!m || !m->ram) return false;
    uint32_t fb_pa = vm_guest_fb_pa(m->ram_base, m->ram_size);
    if (!fb_pa) return false;

    uint32_t blob[VM_GUEST_BLOB_BYTES / 4];
    memset(blob, 0, sizeof blob);
    emitter_t e = { blob, 0 };

    static const char banner[] =
        "iOS3-VM: S5L8900 running on this device.\r\n"
        "ARMv6 interpreter -> system bus -> guest DRAM.\r\n"
        "Painting a 320x480 32bpp framebuffer.\r\n";
    static const char msg[] = "guest: frame ";

    const uint32_t str_pa  = m->ram_base + STR_OFF;
    const uint32_t msg_pa  = str_pa + (uint32_t)sizeof banner;   /* incl. NUL */

    /* --- setup ------------------------------------------------------- */
    emit_ldr_lit(&e, 7, LIT_WORD + 0);                    /* ldr r7,=UART0  */
    emit_ldr_lit(&e, 8, LIT_WORD + 1);                    /* ldr r8,=FB     */
    emit(&e, dp_imm(C_AL, OP_MOV, 0, 0,  9, 15, 0x50));   /* mov r9,#320    */
    emit(&e, dp_imm(C_AL, OP_MOV, 0, 0, 10, 15, 0x78));   /* mov r10,#480   */
    emit(&e, dp_imm(C_AL, OP_MOV, 0, 0,  4,  0, 0));      /* mov r4,#0      */
    emit_ldr_lit(&e, 6, LIT_WORD + 2);                    /* ldr r6,=banner */
    emit_puts(&e);

    /* --- one frame --------------------------------------------------- */
    unsigned l_frame = e.n;
    emit(&e, dp_reg(C_AL, OP_MOV, 0, 0, 0, 8, SH_LSL, 0));      /* mov r0,r8     */
    emit(&e, dp_imm(C_AL, OP_MOV, 0, 0, 3, 0, 0));              /* mov r3,#0     */

    unsigned l_y = e.n;
    emit(&e, dp_reg(C_AL, OP_ADD, 0, 3, 11, 4, SH_LSL, 0));     /* add r11,r3,r4 */
    emit(&e, dp_imm(C_AL, OP_AND, 0, 11, 11, 0, 0xffu));        /* and r11,#0xff */
    emit(&e, dp_reg(C_AL, OP_MOV, 0, 0, 11, 11, SH_LSL, 8));    /* lsl r11,#8    */
    emit(&e, dp_imm(C_AL, OP_ORR, 0, 11, 11, 4, 0xffu));        /* orr #ff000000 */
    emit(&e, dp_imm(C_AL, OP_MOV, 0, 0, 2, 0, 0));              /* mov r2,#0     */

    unsigned l_x = e.n;
    emit(&e, dp_reg(C_AL, OP_EOR, 0, 2, 5, 3, SH_LSL, 0));      /* eor r5,r2,r3  */
    emit(&e, dp_imm(C_AL, OP_AND, 0, 5, 5, 0, 0xffu));          /* and r5,#0xff  */
    emit(&e, dp_reg(C_AL, OP_ADD, 0, 2, 6, 4, SH_LSL, 0));      /* add r6,r2,r4  */
    emit(&e, dp_imm(C_AL, OP_AND, 0, 6, 6, 0, 0xffu));          /* and r6,#0xff  */
    emit(&e, dp_reg(C_AL, OP_ORR, 0, 5, 5, 6, SH_LSL, 16));     /* orr r5,r6<<16 */
    emit(&e, dp_reg(C_AL, OP_ORR, 0, 5, 5, 11, SH_LSL, 0));     /* orr r5,r11    */
    emit(&e, ldst(C_AL, false, false, false, true, false, 0, 5, 4)); /* str r5,[r0],#4 */
    emit(&e, dp_imm(C_AL, OP_ADD, 0, 2, 2, 0, 1));              /* add r2,#1     */
    emit(&e, dp_reg(C_AL, OP_CMP, 1, 2, 0, 9, SH_LSL, 0));      /* cmp r2,r9     */
    emit_b(&e, C_NE, l_x);

    emit(&e, dp_imm(C_AL, OP_ADD, 0, 3, 3, 0, 1));              /* add r3,#1     */
    emit(&e, dp_reg(C_AL, OP_CMP, 1, 3, 0, 10, SH_LSL, 0));     /* cmp r3,r10    */
    emit_b(&e, C_NE, l_y);

    /* --- end of frame: announce it on the UART ----------------------- */
    emit(&e, dp_imm(C_AL, OP_ADD, 0, 4, 4, 0, 1));              /* add r4,#1     */
    emit_ldr_lit(&e, 6, LIT_WORD + 3);                          /* ldr r6,=msg   */
    emit_puts(&e);
    emit_hex_digit(&e, 4);
    emit_hex_digit(&e, 0);
    emit(&e, dp_imm(C_AL, OP_MOV, 0, 0, 5, 0, '\n'));
    emit(&e, ldst(C_AL, false, false, true, true, false, 7, 5, UART_UTXH));
    emit_b(&e, C_AL, l_frame);

    /* The literal pool must not have been overwritten by the code. */
    if (e.n > LIT_WORD) return false;

    blob[LIT_WORD + 0] = S5L8900_UART0_BASE;
    blob[LIT_WORD + 1] = fb_pa;
    blob[LIT_WORD + 2] = str_pa;
    blob[LIT_WORD + 3] = msg_pa;

    /* Strings live past the pool, addressed absolutely through it. */
    if (STR_OFF + sizeof banner + sizeof msg > VM_GUEST_BLOB_BYTES) return false;
    memcpy((uint8_t *)blob + STR_OFF, banner, sizeof banner);
    memcpy((uint8_t *)blob + STR_OFF + sizeof banner, msg, sizeof msg);

    s5l8900_load(m, m->ram_base, blob, sizeof blob);

#ifdef S5L8900_CLCD_BASE
    /*
     * Stand in for iBoot: program display window 0 to point at the framebuffer
     * this payload paints, in the format it paints (32bpp, BGRA). The emulated
     * display controller then reports a real, guest-owned framebuffer that the
     * app scans out through vm_guest_display() — the same handoff a real iBoot
     * leaves for IOMobileFramebuffer to adopt. The interrupt mask is untouched,
     * so no frame IRQ can reach this vector-less payload.
     */
    s5l_clcd_seed_window0(&m->clcd, fb_pa, VM_FB_WIDTH, VM_FB_HEIGHT,
                          VM_FB_WIDTH * VM_FB_BPP, CLCD_FMT_32BPP,
                          CLCD_ORDER_BGRA);
#endif

    /* Start in a privileged mode with interrupts masked: this payload has no
     * vector table, so an interrupt would branch it into unwritten memory. */
    arm_reset(&m->cpu, &m->bus);
    m->cpu.r[15] = m->ram_base;
    return true;
}

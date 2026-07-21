/*
 * iOS3-VM — AArch64 instruction emitter.
 *
 * The code-generation half of the ARMv6 -> arm64 dynamic recompiler described
 * in docs/dynarec.md. This file knows nothing about ARMv6, about the guest, or
 * about executable memory: it turns calls into 32-bit AArch64 instruction
 * words in a caller-supplied buffer. That makes it pure computation, so it
 * builds and unit-tests byte-exactly on the x86 Windows dev box even though the
 * emitted code can only *run* on an arm64 host (see core/tests/test_a64_emit.c).
 *
 * Rules this file follows, in order of importance:
 *
 *   1. Never guess an encoding. Only instructions whose encodings have been
 *      derived from the ARMv8-A field layouts *and* checked against known-good
 *      assembler output are here. Anything else is absent, not approximated.
 *   2. Never emit garbage for a bad operand. Every function validates its
 *      operands; an unencodable request sets `e->bad` and emits nothing, so a
 *      translator bug surfaces as a refusal to translate rather than as a
 *      wrong instruction in the code cache.
 *   3. Buffer overrun is a latched flag, not a crash: the translator checks
 *      a64_ok() once at the end of a block instead of after every emit.
 *
 * Register numbering is the architectural one: 0..30 are X0..X30/W0..W30 and
 * 31 is either the zero register or SP depending on the instruction. The two
 * names below are for readability only; callers must know which an encoding
 * takes (add/sub-immediate and load/store base take SP, everything else here
 * takes ZR).
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_A64_EMIT_H
#define IOS3VM_A64_EMIT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define A64_ZR 31
#define A64_SP 31

/* Operand size selector. Passed as `sf` throughout: 0 = 32-bit W, 1 = 64-bit X. */
#define A64_W 0
#define A64_X 1

/*
 * Condition codes. Deliberately identical in value and meaning to the ARMv6
 * condition field, which is what makes a guest conditional instruction one
 * `B.<cond>` rather than a decode table (docs/dynarec.md §5.1).
 */
typedef enum {
    A64_EQ = 0x0, A64_NE = 0x1, A64_CS = 0x2, A64_CC = 0x3,
    A64_MI = 0x4, A64_PL = 0x5, A64_VS = 0x6, A64_VC = 0x7,
    A64_HI = 0x8, A64_LS = 0x9, A64_GE = 0xa, A64_LT = 0xb,
    A64_GT = 0xc, A64_LE = 0xd, A64_AL = 0xe, A64_NV = 0xf
} a64_cond_t;

/* Invert a condition. AL/NV have no inverse and are rejected by the caller. */
static inline a64_cond_t a64_invert_cond(a64_cond_t c) { return (a64_cond_t)(c ^ 1u); }

/* Shift applied to the second operand of a shifted-register instruction. */
typedef enum { A64_LSL = 0, A64_LSR = 1, A64_ASR = 2, A64_ROR = 3 } a64_shift_t;

/*
 * Index/extend option for register-offset loads and stores.
 * These are the `option` field values; UXTX is spelled LSL by the assembler.
 */
typedef enum {
    A64_EXT_UXTW = 2, A64_EXT_LSL = 3, A64_EXT_SXTW = 6, A64_EXT_SXTX = 7
} a64_ext_t;

/* Access size for load/store, as the `size` field: 0=byte 1=half 2=word 3=dword. */
typedef enum { A64_SZ_B = 0, A64_SZ_H = 1, A64_SZ_W = 2, A64_SZ_D = 3 } a64_size_t;

typedef struct {
    uint32_t *buf;      /* caller-owned instruction buffer                     */
    size_t    cap;      /* capacity in 32-bit words                            */
    size_t    n;        /* words emitted so far                                */
    bool      overflow; /* an emit was dropped because the buffer was full     */
    bool      bad;      /* an operand was unencodable; see rule 2 above        */
} a64_emit_t;

void a64_init(a64_emit_t *e, uint32_t *buf, size_t cap_words);

/* True while nothing has gone wrong. Check once per block, not per instruction. */
static inline bool a64_ok(const a64_emit_t *e) { return !e->overflow && !e->bad; }

/* Byte size of what has been emitted. */
static inline size_t a64_size_bytes(const a64_emit_t *e) { return e->n * 4u; }

/* Emit a pre-encoded word. Used by tests and by hand-written stubs. */
void a64_word(a64_emit_t *e, uint32_t w);

/* ------------------------------------------------------------ move wide ---- */
/* `shift` is a bit position: 0/16 for sf==0, 0/16/32/48 for sf==1.            */
void a64_movz(a64_emit_t *e, int sf, unsigned rd, uint32_t imm16, unsigned shift);
void a64_movn(a64_emit_t *e, int sf, unsigned rd, uint32_t imm16, unsigned shift);
void a64_movk(a64_emit_t *e, int sf, unsigned rd, uint32_t imm16, unsigned shift);

/*
 * Materialise an arbitrary constant, choosing MOVZ or MOVN as the base and
 * MOVK for the remaining halfwords. At most 2 instructions for sf==0 and 4 for
 * sf==1. The sequence is deterministic so it can be asserted byte-exactly.
 */
void a64_mov_imm(a64_emit_t *e, int sf, unsigned rd, uint64_t value);
/* How many words a64_mov_imm would emit, without emitting. */
unsigned a64_mov_imm_words(int sf, uint64_t value);

/* MOV Rd, Rm — the ORR Rd, ZR, Rm alias. Neither operand may be SP. */
void a64_mov_reg(a64_emit_t *e, int sf, unsigned rd, unsigned rm);
/* MOV Rd, Rn where either may be SP — the ADD Rd, Rn, #0 alias. */
void a64_mov_sp(a64_emit_t *e, int sf, unsigned rd, unsigned rn);

/* ------------------------------------------- add / subtract, immediate ----- */
/* imm12 must be 0..4095; `shift12` left-shifts it by 12. Rd/Rn may be SP.     */
bool a64_addsub_imm_fits(uint64_t imm);   /* encodable with shift12 = 0 or 1?  */
void a64_add_imm (a64_emit_t *e, int sf, unsigned rd, unsigned rn, uint32_t imm12, bool shift12);
void a64_adds_imm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, uint32_t imm12, bool shift12);
void a64_sub_imm (a64_emit_t *e, int sf, unsigned rd, unsigned rn, uint32_t imm12, bool shift12);
void a64_subs_imm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, uint32_t imm12, bool shift12);
void a64_cmp_imm (a64_emit_t *e, int sf, unsigned rn, uint32_t imm12, bool shift12);
void a64_cmn_imm (a64_emit_t *e, int sf, unsigned rn, uint32_t imm12, bool shift12);

/* -------------------------------------- add / subtract, shifted register --- */
/* `shift` is LSL/LSR/ASR only (ROR is not encodable here); amount 0..31/0..63. */
void a64_add_reg (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_adds_reg(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_sub_reg (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_subs_reg(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_cmp_reg (a64_emit_t *e, int sf, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_cmn_reg (a64_emit_t *e, int sf, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_neg_reg (a64_emit_t *e, int sf, unsigned rd, unsigned rm);

/* ------------------------------------- add / subtract with carry ----------- */
void a64_adc (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm);
void a64_adcs(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm);
void a64_sbc (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm);
void a64_sbcs(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm);

/* ------------------------------------------- logical, shifted register ----- */
void a64_and_reg (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_ands_reg(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_bic_reg (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_bics_reg(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_orr_reg (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_orn_reg (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_eor_reg (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_eon_reg (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_tst_reg (a64_emit_t *e, int sf, unsigned rn, unsigned rm, a64_shift_t sh, unsigned amt);
void a64_mvn_reg (a64_emit_t *e, int sf, unsigned rd, unsigned rm);

/* ------------------------------------------------- logical, immediate ------ */
/*
 * The "bitmask immediate": a rotated run of ones replicated to fill the
 * register. Only a minority of 32/64-bit values are encodable, so callers must
 * ask first with a64_bitmask_imm() and materialise the constant otherwise.
 * Returns false and leaves *enc untouched when the value is not encodable.
 * *enc packs N:immr:imms as (N << 12) | (immr << 6) | imms.
 */
bool a64_bitmask_imm(int sf, uint64_t value, uint32_t *enc);
void a64_and_imm (a64_emit_t *e, int sf, unsigned rd, unsigned rn, uint64_t imm);
void a64_ands_imm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, uint64_t imm);
void a64_orr_imm (a64_emit_t *e, int sf, unsigned rd, unsigned rn, uint64_t imm);
void a64_eor_imm (a64_emit_t *e, int sf, unsigned rd, unsigned rn, uint64_t imm);
void a64_tst_imm (a64_emit_t *e, int sf, unsigned rn, uint64_t imm);

/* -------------------------------------------------- variable shifts -------- */
void a64_lslv(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm);
void a64_lsrv(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm);
void a64_asrv(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm);
void a64_rorv(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm);
void a64_clz (a64_emit_t *e, int sf, unsigned rd, unsigned rn);

/* ------------------------------------------------------- bitfield ---------- */
void a64_bfm (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned immr, unsigned imms);
void a64_sbfm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned immr, unsigned imms);
void a64_ubfm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned immr, unsigned imms);
/* Aliases. `width` is 1..(regsize - lsb). */
void a64_lsl_imm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned shift);
void a64_lsr_imm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned shift);
void a64_asr_imm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned shift);
void a64_ubfx   (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned lsb, unsigned width);
void a64_sbfx   (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned lsb, unsigned width);
void a64_bfi    (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned lsb, unsigned width);

/* ------------------------------------------------------- conditional ------- */
void a64_csel (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_cond_t c);
void a64_csinc(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, a64_cond_t c);
void a64_cset (a64_emit_t *e, int sf, unsigned rd, a64_cond_t c);

/* ------------------------------------------------------- multiply ---------- */
void a64_madd(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, unsigned ra);
void a64_mul (a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm);

/* ------------------------------------------------ load / store ------------- */
/*
 * Unsigned scaled immediate offset: LDR/STR Rt, [Rn, #imm].
 * `offset` is in bytes and must be a multiple of (1 << size) and, once scaled,
 * fit in 12 bits. Rn may be SP.
 */
void a64_str_uimm (a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rn, uint32_t offset);
void a64_ldr_uimm (a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rn, uint32_t offset);
/* Sign-extending loads. `sf` selects the destination width (X or W). */
void a64_ldrs_uimm(a64_emit_t *e, a64_size_t size, int sf, unsigned rt, unsigned rn, uint32_t offset);

/* Unscaled signed 9-bit offset (LDUR/STUR), and the pre/post-indexed forms. */
void a64_stur(a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rn, int32_t off9);
void a64_ldur(a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rn, int32_t off9);
void a64_str_pre  (a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rn, int32_t off9);
void a64_ldr_pre  (a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rn, int32_t off9);
void a64_str_post (a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rn, int32_t off9);
void a64_ldr_post (a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rn, int32_t off9);

/*
 * Register offset: LDR/STR Rt, [Rn, Rm, <ext> #amount].
 * `amount` is 0 or log2(access size); any other value is not encodable.
 * This is the emulator's memory fast path (docs/dynarec.md §6.2).
 */
void a64_str_reg(a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rn, unsigned rm, a64_ext_t ext, unsigned amount);
void a64_ldr_reg(a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rn, unsigned rm, a64_ext_t ext, unsigned amount);

/*
 * Load/store pair. `size` must be A64_SZ_W or A64_SZ_D. `offset` is in bytes,
 * must be a multiple of the access size, and must fit in a signed 7-bit
 * scaled field. Rn may be SP.
 */
void a64_stp     (a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rt2, unsigned rn, int32_t offset);
void a64_ldp     (a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rt2, unsigned rn, int32_t offset);
void a64_stp_pre (a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rt2, unsigned rn, int32_t offset);
void a64_ldp_pre (a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rt2, unsigned rn, int32_t offset);
void a64_stp_post(a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rt2, unsigned rn, int32_t offset);
void a64_ldp_post(a64_emit_t *e, a64_size_t size, unsigned rt, unsigned rt2, unsigned rn, int32_t offset);

/* ------------------------------------------------------------- branches ---- */
/* `off_words` is a signed instruction count relative to the branch itself. */
void a64_b   (a64_emit_t *e, int32_t off_words);
void a64_bl  (a64_emit_t *e, int32_t off_words);
void a64_bcond(a64_emit_t *e, a64_cond_t c, int32_t off_words);
void a64_cbz (a64_emit_t *e, int sf, unsigned rt, int32_t off_words);
void a64_cbnz(a64_emit_t *e, int sf, unsigned rt, int32_t off_words);
void a64_tbz (a64_emit_t *e, unsigned rt, unsigned bit, int32_t off_words);
void a64_tbnz(a64_emit_t *e, unsigned rt, unsigned bit, int32_t off_words);
void a64_br  (a64_emit_t *e, unsigned rn);
void a64_blr (a64_emit_t *e, unsigned rn);
void a64_ret (a64_emit_t *e, unsigned rn);   /* pass 30 for the plain RET */

/*
 * Forward-branch fixup. Emit the branch with off_words == 0, remember the word
 * index it landed at, and later call a64_bind() to point it at the current
 * emission position. The branch kind is recovered from the encoded word, so
 * one function covers B/BL, B.cond, CBZ/CBNZ and TBZ/TBNZ.
 * Sets e->bad if the displacement does not fit the branch's field.
 */
void a64_bind(a64_emit_t *e, size_t branch_word_index);
/* Patch an already-emitted branch word in place; used for chain patching. */
bool a64_patch_branch(uint32_t *insn, int32_t off_words);

/* --------------------------------------------------- system / flags -------- */
/* Generic system-register access. NZCV is (3,3,4,2,0). */
void a64_mrs(a64_emit_t *e, unsigned rt, unsigned op0, unsigned op1, unsigned crn, unsigned crm, unsigned op2);
void a64_msr(a64_emit_t *e, unsigned rt, unsigned op0, unsigned op1, unsigned crn, unsigned crm, unsigned op2);
/* Guest N/Z/C/V live in the host NZCV; these move them in and out. */
void a64_mrs_nzcv(a64_emit_t *e, unsigned rt);
void a64_msr_nzcv(a64_emit_t *e, unsigned rt);

void a64_nop(a64_emit_t *e);
void a64_brk(a64_emit_t *e, uint32_t imm16);

#endif /* IOS3VM_A64_EMIT_H */

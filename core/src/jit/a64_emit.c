/*
 * iOS3-VM — AArch64 instruction emitter.
 *
 * See core/include/a64_emit.h for the contract. Every base opcode below is
 * written as the literal field layout from the ARMv8-A encoding tables, with a
 * known-good disassembly in the comment so the value can be checked by eye and
 * by core/tests/test_a64_emit.c.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "a64_emit.h"

/* ------------------------------------------------------------- primitives */

void a64_init(a64_emit_t *e, uint32_t *buf, size_t cap_words) {
    e->buf = buf; e->cap = cap_words; e->n = 0;
    e->overflow = false; e->bad = false;
}

void a64_word(a64_emit_t *e, uint32_t w) {
    if (e->n >= e->cap) { e->overflow = true; return; }
    e->buf[e->n++] = w;
}

/* Refuse to encode rather than truncate silently. */
static bool bad_if(a64_emit_t *e, bool cond) {
    if (cond) e->bad = true;
    return cond;
}
static bool reg_ok(unsigned r) { return r < 32u; }
static bool regs_ok3(a64_emit_t *e, unsigned a, unsigned b, unsigned c) {
    return !bad_if(e, !reg_ok(a) || !reg_ok(b) || !reg_ok(c));
}
static bool regs_ok2(a64_emit_t *e, unsigned a, unsigned b) {
    return !bad_if(e, !reg_ok(a) || !reg_ok(b));
}

/* ------------------------------------------------------------- move wide */
/*
 * sf | opc(2) | 100101 | hw(2) | imm16 | Rd
 * opc: 00 MOVN, 10 MOVZ, 11 MOVK.   movz w0,#42 -> 0x52800540
 */
static void movewide(a64_emit_t *e, int sf, unsigned opc, unsigned rd,
                     uint32_t imm16, unsigned shift) {
    if (!regs_ok2(e, rd, 0)) return;
    if (bad_if(e, imm16 > 0xffffu)) return;
    if (bad_if(e, (shift & 15u) != 0 || shift > 48u)) return;
    if (bad_if(e, !sf && shift > 16u)) return;
    a64_word(e, ((uint32_t)(sf & 1) << 31) | (opc << 29) | (0x25u << 23)
                | ((shift / 16u) << 21) | (imm16 << 5) | rd);
}
void a64_movn(a64_emit_t *e, int sf, unsigned rd, uint32_t imm16, unsigned shift)
    { movewide(e, sf, 0u, rd, imm16, shift); }
void a64_movz(a64_emit_t *e, int sf, unsigned rd, uint32_t imm16, unsigned shift)
    { movewide(e, sf, 2u, rd, imm16, shift); }
void a64_movk(a64_emit_t *e, int sf, unsigned rd, uint32_t imm16, unsigned shift)
    { movewide(e, sf, 3u, rd, imm16, shift); }

/*
 * Constant materialisation. Pick MOVN as the base when more halfwords are
 * 0xffff than are 0x0000, MOVZ otherwise, then MOVK whatever is left. This is
 * the classic choice and, crucially for testing, it is deterministic.
 */
static unsigned mov_imm_plan(int sf, uint64_t value, bool *use_movn,
                             unsigned *base_half, uint64_t *halves) {
    unsigned nhalf = sf ? 4u : 2u, i, zeros = 0, ones = 0, count = 0;
    if (!sf) value &= 0xffffffffu;
    for (i = 0; i < nhalf; i++) {
        halves[i] = (value >> (16u * i)) & 0xffffu;
        if (halves[i] == 0u)      zeros++;
        else if (halves[i] == 0xffffu) ones++;
    }
    *use_movn = ones > zeros;
    /* The base instruction covers one halfword; MOVK covers the others. */
    *base_half = nhalf;   /* sentinel: "no interesting halfword found" */
    for (i = 0; i < nhalf; i++) {
        uint64_t skip = *use_movn ? 0xffffu : 0u;
        if (halves[i] != skip) { if (*base_half == nhalf) *base_half = i; count++; }
    }
    if (*base_half == nhalf) { *base_half = 0; count = 1; }  /* all-zero / all-ones */
    return count;
}

unsigned a64_mov_imm_words(int sf, uint64_t value) {
    bool movn; unsigned base; uint64_t h[4];
    return mov_imm_plan(sf, value, &movn, &base, h);
}

void a64_mov_imm(a64_emit_t *e, int sf, unsigned rd, uint64_t value) {
    bool movn; unsigned base, i, nhalf = sf ? 4u : 2u; uint64_t h[4];
    (void)mov_imm_plan(sf, value, &movn, &base, h);
    if (movn) {
        /* MOVN writes ~(imm16 << shift); the other halfwords become ones. */
        a64_movn(e, sf, rd, (uint32_t)((~h[base]) & 0xffffu), base * 16u);
    } else {
        a64_movz(e, sf, rd, (uint32_t)h[base], base * 16u);
    }
    for (i = 0; i < nhalf; i++) {
        uint64_t skip = movn ? 0xffffu : 0u;
        if (i == base || h[i] == skip) continue;
        a64_movk(e, sf, rd, (uint32_t)h[i], i * 16u);
    }
}

/* ---------------------------------------------- add / subtract immediate */
/*
 * sf | op | S | 100010 | sh | imm12 | Rn | Rd
 * add x0,x0,#1 -> 0x91000400 ; sub sp,sp,#16 -> 0xd10043ff
 */
static void addsub_imm(a64_emit_t *e, int sf, unsigned op, unsigned S,
                       unsigned rd, unsigned rn, uint32_t imm12, bool sh) {
    if (!regs_ok2(e, rd, rn)) return;
    if (bad_if(e, imm12 > 0xfffu)) return;
    a64_word(e, ((uint32_t)(sf & 1) << 31) | (op << 30) | (S << 29) | (0x22u << 23)
                | ((sh ? 1u : 0u) << 22) | (imm12 << 10) | (rn << 5) | rd);
}
bool a64_addsub_imm_fits(uint64_t imm) {
    return imm <= 0xfffu || ((imm & 0xfffu) == 0 && (imm >> 12) <= 0xfffu);
}
void a64_add_imm (a64_emit_t *e, int sf, unsigned rd, unsigned rn, uint32_t i, bool s){ addsub_imm(e,sf,0,0,rd,rn,i,s); }
void a64_adds_imm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, uint32_t i, bool s){ addsub_imm(e,sf,0,1,rd,rn,i,s); }
void a64_sub_imm (a64_emit_t *e, int sf, unsigned rd, unsigned rn, uint32_t i, bool s){ addsub_imm(e,sf,1,0,rd,rn,i,s); }
void a64_subs_imm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, uint32_t i, bool s){ addsub_imm(e,sf,1,1,rd,rn,i,s); }
void a64_cmp_imm (a64_emit_t *e, int sf, unsigned rn, uint32_t i, bool s){ addsub_imm(e,sf,1,1,A64_ZR,rn,i,s); }
void a64_cmn_imm (a64_emit_t *e, int sf, unsigned rn, uint32_t i, bool s){ addsub_imm(e,sf,0,1,A64_ZR,rn,i,s); }

void a64_mov_sp(a64_emit_t *e, int sf, unsigned rd, unsigned rn) {
    a64_add_imm(e, sf, rd, rn, 0, false);
}

/* --------------------------------------- add / subtract shifted register */
/*
 * sf | op | S | 01011 | shift(2) | 0 | Rm | imm6 | Rn | Rd
 * add x0,x1,x2 -> 0x8b020020 ; cmp x0,x1 -> 0xeb01001f
 */
static void addsub_reg(a64_emit_t *e, int sf, unsigned op, unsigned S,
                       unsigned rd, unsigned rn, unsigned rm,
                       a64_shift_t sh, unsigned amt) {
    if (!regs_ok3(e, rd, rn, rm)) return;
    if (bad_if(e, sh == A64_ROR)) return;            /* not encodable here */
    if (bad_if(e, amt > (sf ? 63u : 31u))) return;
    a64_word(e, ((uint32_t)(sf & 1) << 31) | (op << 30) | (S << 29) | (0x0bu << 24)
                | ((unsigned)sh << 22) | (rm << 16) | (amt << 10) | (rn << 5) | rd);
}
void a64_add_reg (a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_shift_t s,unsigned a){ addsub_reg(e,sf,0,0,d,n,m,s,a); }
void a64_adds_reg(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_shift_t s,unsigned a){ addsub_reg(e,sf,0,1,d,n,m,s,a); }
void a64_sub_reg (a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_shift_t s,unsigned a){ addsub_reg(e,sf,1,0,d,n,m,s,a); }
void a64_subs_reg(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_shift_t s,unsigned a){ addsub_reg(e,sf,1,1,d,n,m,s,a); }
void a64_cmp_reg (a64_emit_t *e,int sf,unsigned n,unsigned m,a64_shift_t s,unsigned a){ addsub_reg(e,sf,1,1,A64_ZR,n,m,s,a); }
void a64_cmn_reg (a64_emit_t *e,int sf,unsigned n,unsigned m,a64_shift_t s,unsigned a){ addsub_reg(e,sf,0,1,A64_ZR,n,m,s,a); }
void a64_neg_reg (a64_emit_t *e,int sf,unsigned d,unsigned m){ addsub_reg(e,sf,1,0,d,A64_ZR,m,A64_LSL,0); }

/* --------------------------------------------- add / subtract with carry */
/*
 * sf | op | S | 11010000 | Rm | 000000 | Rn | Rd
 * adc w0,w1,w2 -> 0x1a020020
 */
static void adcsbc(a64_emit_t *e, int sf, unsigned op, unsigned S,
                   unsigned rd, unsigned rn, unsigned rm) {
    if (!regs_ok3(e, rd, rn, rm)) return;
    a64_word(e, ((uint32_t)(sf & 1) << 31) | (op << 30) | (S << 29) | (0xd0u << 21)
                | (rm << 16) | (rn << 5) | rd);
}
void a64_adc (a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m){ adcsbc(e,sf,0,0,d,n,m); }
void a64_adcs(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m){ adcsbc(e,sf,0,1,d,n,m); }
void a64_sbc (a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m){ adcsbc(e,sf,1,0,d,n,m); }
void a64_sbcs(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m){ adcsbc(e,sf,1,1,d,n,m); }

/* --------------------------------------------- logical, shifted register */
/*
 * sf | opc(2) | 01010 | shift(2) | N | Rm | imm6 | Rn | Rd
 * opc/N: 00/0 AND, 00/1 BIC, 01/0 ORR, 01/1 ORN, 10/0 EOR, 10/1 EON,
 *        11/0 ANDS, 11/1 BICS.      mov x0,x1 == orr x0,xzr,x1 -> 0xaa0103e0
 */
static void logic_reg(a64_emit_t *e, int sf, unsigned opc, unsigned N,
                      unsigned rd, unsigned rn, unsigned rm,
                      a64_shift_t sh, unsigned amt) {
    if (!regs_ok3(e, rd, rn, rm)) return;
    if (bad_if(e, amt > (sf ? 63u : 31u))) return;
    a64_word(e, ((uint32_t)(sf & 1) << 31) | (opc << 29) | (0x0au << 24)
                | ((unsigned)sh << 22) | (N << 21) | (rm << 16) | (amt << 10)
                | (rn << 5) | rd);
}
void a64_and_reg (a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_shift_t s,unsigned a){ logic_reg(e,sf,0,0,d,n,m,s,a); }
void a64_bic_reg (a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_shift_t s,unsigned a){ logic_reg(e,sf,0,1,d,n,m,s,a); }
void a64_orr_reg (a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_shift_t s,unsigned a){ logic_reg(e,sf,1,0,d,n,m,s,a); }
void a64_orn_reg (a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_shift_t s,unsigned a){ logic_reg(e,sf,1,1,d,n,m,s,a); }
void a64_eor_reg (a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_shift_t s,unsigned a){ logic_reg(e,sf,2,0,d,n,m,s,a); }
void a64_eon_reg (a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_shift_t s,unsigned a){ logic_reg(e,sf,2,1,d,n,m,s,a); }
void a64_ands_reg(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_shift_t s,unsigned a){ logic_reg(e,sf,3,0,d,n,m,s,a); }
void a64_bics_reg(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_shift_t s,unsigned a){ logic_reg(e,sf,3,1,d,n,m,s,a); }
void a64_tst_reg (a64_emit_t *e,int sf,unsigned n,unsigned m,a64_shift_t s,unsigned a){ logic_reg(e,sf,3,0,A64_ZR,n,m,s,a); }
void a64_mvn_reg (a64_emit_t *e,int sf,unsigned d,unsigned m){ logic_reg(e,sf,1,1,d,A64_ZR,m,A64_LSL,0); }
void a64_mov_reg (a64_emit_t *e,int sf,unsigned d,unsigned m){ logic_reg(e,sf,1,0,d,A64_ZR,m,A64_LSL,0); }

/* ---------------------------------------------------- logical immediate */
/*
 * The bitmask immediate encodes a value of the form
 *     Replicate(ROR(Ones(S+1), R), 64/esize)
 * with esize in {2,4,8,16,32,64}. The search below is the standard one: find
 * the smallest element the value is periodic in, check that element is a
 * contiguous (possibly rotated) run of ones, and derive N/immr/imms from it.
 */
static unsigned ctz64(uint64_t v) { unsigned n = 0; while (!(v & 1u)) { v >>= 1; n++; } return n; }
static unsigned cto64(uint64_t v) { unsigned n = 0; while (v & 1u) { v >>= 1; n++; } return n; }
static unsigned clo64(uint64_t v) { unsigned n = 0; while (v >> 63) { v <<= 1; n++; } return n; }
/* All ones from bit 0 up: 0b0..01..1, and non-zero. */
static bool is_mask(uint64_t v) { return v != 0 && ((v + 1u) & v) == 0; }
/* A single contiguous run of ones at any position. */
static bool is_shifted_mask(uint64_t v) { return v != 0 && is_mask((v - 1u) | v); }

bool a64_bitmask_imm(int sf, uint64_t value, uint32_t *enc) {
    unsigned regsize = sf ? 64u : 32u;
    uint64_t imm = value, mask;
    unsigned size, cto, i, immr, n;
    uint64_t nimms;

    if (regsize != 64u) {
        if (value >> 32) return false;
        imm &= 0xffffffffu;
    }
    if (imm == 0u) return false;
    if (regsize == 64u ? (imm == ~(uint64_t)0) : (imm == 0xffffffffu)) return false;

    /* Smallest element size the value is periodic in. */
    size = regsize;
    do {
        size /= 2u;
        mask = (size == 64u) ? ~(uint64_t)0 : (((uint64_t)1 << size) - 1u);
        if ((imm & mask) != ((imm >> size) & mask)) { size *= 2u; break; }
    } while (size > 2u);

    mask = (size == 64u) ? ~(uint64_t)0 : (((uint64_t)1 << size) - 1u);
    imm &= mask;

    if (is_shifted_mask(imm)) {
        i   = ctz64(imm);
        cto = cto64(imm >> i);
    } else {
        imm |= ~mask;
        if (!is_shifted_mask(~imm)) return false;
        { unsigned clo = clo64(imm);
          i   = 64u - clo;
          cto = clo + cto64(imm) - (64u - size); }
    }

    immr  = (size - i) % size;
    nimms = (~(uint64_t)(size - 1u)) << 1;
    nimms |= (uint64_t)(cto - 1u);
    n = (unsigned)((nimms >> 6) & 1u) ^ 1u;

    if (!sf && n) return false;        /* 32-bit forms require N == 0 */
    *enc = (n << 12) | (immr << 6) | (unsigned)(nimms & 0x3fu);
    return true;
}

/*
 * sf | opc(2) | 100100 | N | immr | imms | Rn | Rd
 * and w0,w0,#0xff -> 0x12001c00 ; and x0,x0,#0xfff -> 0x92402c00
 */
static void logic_imm(a64_emit_t *e, int sf, unsigned opc,
                      unsigned rd, unsigned rn, uint64_t imm) {
    uint32_t enc;
    if (!regs_ok2(e, rd, rn)) return;
    if (bad_if(e, !a64_bitmask_imm(sf, imm, &enc))) return;
    a64_word(e, ((uint32_t)(sf & 1) << 31) | (opc << 29) | (0x24u << 23)
                | (((enc >> 12) & 1u) << 22) | (((enc >> 6) & 0x3fu) << 16)
                | ((enc & 0x3fu) << 10) | (rn << 5) | rd);
}
void a64_and_imm (a64_emit_t *e,int sf,unsigned d,unsigned n,uint64_t i){ logic_imm(e,sf,0,d,n,i); }
void a64_orr_imm (a64_emit_t *e,int sf,unsigned d,unsigned n,uint64_t i){ logic_imm(e,sf,1,d,n,i); }
void a64_eor_imm (a64_emit_t *e,int sf,unsigned d,unsigned n,uint64_t i){ logic_imm(e,sf,2,d,n,i); }
void a64_ands_imm(a64_emit_t *e,int sf,unsigned d,unsigned n,uint64_t i){ logic_imm(e,sf,3,d,n,i); }
void a64_tst_imm (a64_emit_t *e,int sf,unsigned n,uint64_t i){ logic_imm(e,sf,3,A64_ZR,n,i); }

/* --------------------------------------------------------- data proc, 2 */
/*
 * sf | 0 | 0 | 11010110 | Rm | 0010 | op2(2) | Rn | Rd
 * lsl w0,w1,w2 (LSLV) -> 0x1ac22020
 */
static void shift_var(a64_emit_t *e, int sf, unsigned op2,
                      unsigned rd, unsigned rn, unsigned rm) {
    if (!regs_ok3(e, rd, rn, rm)) return;
    a64_word(e, ((uint32_t)(sf & 1) << 31) | (0xd6u << 21) | (rm << 16)
                | (0x2u << 12) | (op2 << 10) | (rn << 5) | rd);
}
void a64_lslv(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m){ shift_var(e,sf,0,d,n,m); }
void a64_lsrv(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m){ shift_var(e,sf,1,d,n,m); }
void a64_asrv(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m){ shift_var(e,sf,2,d,n,m); }
void a64_rorv(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m){ shift_var(e,sf,3,d,n,m); }

/* sf | 1 | 0 | 11010110 | 00000 | 00010 0 | Rn | Rd ; clz w0,w1 -> 0x5ac01020 */
void a64_clz(a64_emit_t *e, int sf, unsigned rd, unsigned rn) {
    if (!regs_ok2(e, rd, rn)) return;
    a64_word(e, ((uint32_t)(sf & 1) << 31) | (1u << 30) | (0xd6u << 21)
                | (0x4u << 10) | (rn << 5) | rd);
}

/* ------------------------------------------------------------- bitfield */
/*
 * sf | opc(2) | 100110 | N | immr | imms | Rn | Rd
 * opc: 00 SBFM, 01 BFM, 10 UBFM. N must equal sf.
 * lsr w0,w1,#4 -> 0x53047c20 ; lsl w0,w1,#4 -> 0x531c6c20
 */
static void bitfield(a64_emit_t *e, int sf, unsigned opc, unsigned rd,
                     unsigned rn, unsigned immr, unsigned imms) {
    unsigned lim = sf ? 63u : 31u;
    if (!regs_ok2(e, rd, rn)) return;
    if (bad_if(e, immr > lim || imms > lim)) return;
    a64_word(e, ((uint32_t)(sf & 1) << 31) | (opc << 29) | (0x26u << 23)
                | ((uint32_t)(sf & 1) << 22) | (immr << 16) | (imms << 10)
                | (rn << 5) | rd);
}
void a64_sbfm(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned r,unsigned s){ bitfield(e,sf,0,d,n,r,s); }
void a64_bfm (a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned r,unsigned s){ bitfield(e,sf,1,d,n,r,s); }
void a64_ubfm(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned r,unsigned s){ bitfield(e,sf,2,d,n,r,s); }

void a64_lsl_imm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned sh) {
    unsigned rs = sf ? 64u : 32u;
    if (bad_if(e, sh >= rs)) return;
    a64_ubfm(e, sf, rd, rn, (rs - sh) % rs, rs - 1u - sh);
}
void a64_lsr_imm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned sh) {
    unsigned rs = sf ? 64u : 32u;
    if (bad_if(e, sh >= rs)) return;
    a64_ubfm(e, sf, rd, rn, sh, rs - 1u);
}
void a64_asr_imm(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned sh) {
    unsigned rs = sf ? 64u : 32u;
    if (bad_if(e, sh >= rs)) return;
    a64_sbfm(e, sf, rd, rn, sh, rs - 1u);
}
void a64_ubfx(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned lsb, unsigned w) {
    unsigned rs = sf ? 64u : 32u;
    if (bad_if(e, w == 0 || lsb >= rs || lsb + w > rs)) return;
    a64_ubfm(e, sf, rd, rn, lsb, lsb + w - 1u);
}
void a64_sbfx(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned lsb, unsigned w) {
    unsigned rs = sf ? 64u : 32u;
    if (bad_if(e, w == 0 || lsb >= rs || lsb + w > rs)) return;
    a64_sbfm(e, sf, rd, rn, lsb, lsb + w - 1u);
}
void a64_bfi(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned lsb, unsigned w) {
    unsigned rs = sf ? 64u : 32u;
    if (bad_if(e, w == 0 || lsb >= rs || lsb + w > rs)) return;
    a64_bfm(e, sf, rd, rn, (rs - lsb) % rs, w - 1u);
}

/* ---------------------------------------------------------- conditional */
/*
 * sf | 0 | 0 | 11010100 | Rm | cond | op2(2) | Rn | Rd
 * op2: 00 CSEL, 01 CSINC.       cset w0,eq == csinc w0,wzr,wzr,ne -> 0x1a9f17e0
 */
static void condsel(a64_emit_t *e, int sf, unsigned op2, unsigned rd,
                    unsigned rn, unsigned rm, a64_cond_t c) {
    if (!regs_ok3(e, rd, rn, rm)) return;
    if (bad_if(e, (unsigned)c > 15u)) return;
    a64_word(e, ((uint32_t)(sf & 1) << 31) | (0xd4u << 21) | (rm << 16)
                | ((unsigned)c << 12) | (op2 << 10) | (rn << 5) | rd);
}
void a64_csel (a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_cond_t c){ condsel(e,sf,0,d,n,m,c); }
void a64_csinc(a64_emit_t *e,int sf,unsigned d,unsigned n,unsigned m,a64_cond_t c){ condsel(e,sf,1,d,n,m,c); }
void a64_cset (a64_emit_t *e,int sf,unsigned d,a64_cond_t c) {
    if (bad_if(e, c == A64_AL || c == A64_NV)) return;
    condsel(e, sf, 1, d, A64_ZR, A64_ZR, a64_invert_cond(c));
}

/* ------------------------------------------------------------ multiply */
/* sf | 00 | 11011 | 000 | Rm | 0 | Ra | Rn | Rd ; mul w0,w1,w2 -> 0x1b027c20 */
void a64_madd(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm, unsigned ra) {
    if (!regs_ok3(e, rd, rn, rm) || !regs_ok2(e, ra, 0)) return;
    a64_word(e, ((uint32_t)(sf & 1) << 31) | (0x1bu << 24) | (rm << 16)
                | (ra << 10) | (rn << 5) | rd);
}
void a64_mul(a64_emit_t *e, int sf, unsigned rd, unsigned rn, unsigned rm) {
    a64_madd(e, sf, rd, rn, rm, A64_ZR);
}

/* --------------------------------------------------------- load / store */
/*
 * Unsigned immediate:  size | 111 | V=0 | 01 | opc(2) | imm12 | Rn | Rt
 * ldr w0,[x1] -> 0xb9400020 ; str x0,[x1] -> 0xf9000020
 */
static void ldst_uimm(a64_emit_t *e, a64_size_t size, unsigned opc,
                      unsigned rt, unsigned rn, uint32_t offset) {
    uint32_t scaled = offset >> (unsigned)size;
    if (!regs_ok2(e, rt, rn)) return;
    if (bad_if(e, (offset & ((1u << (unsigned)size) - 1u)) != 0)) return;
    if (bad_if(e, scaled > 0xfffu)) return;
    a64_word(e, ((uint32_t)size << 30) | (0x39u << 24) | (opc << 22)
                | (scaled << 10) | (rn << 5) | rt);
}
void a64_str_uimm(a64_emit_t *e, a64_size_t s, unsigned rt, unsigned rn, uint32_t o)
    { ldst_uimm(e, s, 0u, rt, rn, o); }
void a64_ldr_uimm(a64_emit_t *e, a64_size_t s, unsigned rt, unsigned rn, uint32_t o)
    { ldst_uimm(e, s, 1u, rt, rn, o); }
void a64_ldrs_uimm(a64_emit_t *e, a64_size_t s, int sf, unsigned rt, unsigned rn, uint32_t o) {
    /* opc 10 -> 64-bit destination, opc 11 -> 32-bit destination. There is no
     * 32-bit-destination sign-extending doubleword load. */
    if (bad_if(e, s == A64_SZ_D || (s == A64_SZ_W && !sf))) return;
    ldst_uimm(e, s, sf ? 2u : 3u, rt, rn, o);
}

/*
 * Unscaled / indexed: size | 111 | V=0 | 00 | opc(2) | 0 | imm9 | op2 | Rn | Rt
 * op2: 00 unscaled (LDUR/STUR), 01 post-index, 11 pre-index.
 * str x30,[sp,#-16]! -> 0xf81f0ffe ; ldur x0,[x1,#-8] -> 0xf85f8020
 */
static void ldst_imm9(a64_emit_t *e, a64_size_t size, unsigned opc, unsigned op2,
                      unsigned rt, unsigned rn, int32_t off9) {
    if (!regs_ok2(e, rt, rn)) return;
    if (bad_if(e, off9 < -256 || off9 > 255)) return;
    a64_word(e, ((uint32_t)size << 30) | (0x38u << 24) | (opc << 22)
                | (((uint32_t)off9 & 0x1ffu) << 12) | (op2 << 10) | (rn << 5) | rt);
}
void a64_stur    (a64_emit_t *e,a64_size_t s,unsigned t,unsigned n,int32_t o){ ldst_imm9(e,s,0,0,t,n,o); }
void a64_ldur    (a64_emit_t *e,a64_size_t s,unsigned t,unsigned n,int32_t o){ ldst_imm9(e,s,1,0,t,n,o); }
void a64_str_post(a64_emit_t *e,a64_size_t s,unsigned t,unsigned n,int32_t o){ ldst_imm9(e,s,0,1,t,n,o); }
void a64_ldr_post(a64_emit_t *e,a64_size_t s,unsigned t,unsigned n,int32_t o){ ldst_imm9(e,s,1,1,t,n,o); }
void a64_str_pre (a64_emit_t *e,a64_size_t s,unsigned t,unsigned n,int32_t o){ ldst_imm9(e,s,0,3,t,n,o); }
void a64_ldr_pre (a64_emit_t *e,a64_size_t s,unsigned t,unsigned n,int32_t o){ ldst_imm9(e,s,1,3,t,n,o); }

/*
 * Register offset: size | 111 | V=0 | 00 | opc(2) | 1 | Rm | option | S | 10 | Rn | Rt
 * ldr w0,[x1,x2] -> 0xb8626820
 */
static void ldst_reg(a64_emit_t *e, a64_size_t size, unsigned opc, unsigned rt,
                     unsigned rn, unsigned rm, a64_ext_t ext, unsigned amount) {
    unsigned S;
    if (!regs_ok3(e, rt, rn, rm)) return;
    if (bad_if(e, ext != A64_EXT_UXTW && ext != A64_EXT_LSL
                  && ext != A64_EXT_SXTW && ext != A64_EXT_SXTX)) return;
    if (bad_if(e, amount != 0 && amount != (unsigned)size)) return;
    S = (amount != 0) ? 1u : 0u;
    a64_word(e, ((uint32_t)size << 30) | (0x38u << 24) | (opc << 22) | (1u << 21)
                | (rm << 16) | ((unsigned)ext << 13) | (S << 12) | (2u << 10)
                | (rn << 5) | rt);
}
void a64_str_reg(a64_emit_t *e,a64_size_t s,unsigned t,unsigned n,unsigned m,a64_ext_t x,unsigned a)
    { ldst_reg(e,s,0,t,n,m,x,a); }
void a64_ldr_reg(a64_emit_t *e,a64_size_t s,unsigned t,unsigned n,unsigned m,a64_ext_t x,unsigned a)
    { ldst_reg(e,s,1,t,n,m,x,a); }

/*
 * Pair: opc(2) | 101 | V=0 | mode(3) | L | imm7 | Rt2 | Rn | Rt
 * mode: 001 post-index, 010 signed offset, 011 pre-index.
 * stp x29,x30,[sp,#-16]! -> 0xa9bf7bfd ; ldp x29,x30,[sp],#16 -> 0xa8c17bfd
 */
static void ldst_pair(a64_emit_t *e, a64_size_t size, unsigned mode, unsigned L,
                      unsigned rt, unsigned rt2, unsigned rn, int32_t offset) {
    unsigned scale;
    int32_t  scaled;
    if (!regs_ok3(e, rt, rt2, rn)) return;
    if (bad_if(e, size != A64_SZ_W && size != A64_SZ_D)) return;
    scale = (size == A64_SZ_D) ? 3u : 2u;
    if (bad_if(e, (offset & (int32_t)((1u << scale) - 1u)) != 0)) return;
    scaled = offset >> scale;
    if (bad_if(e, scaled < -64 || scaled > 63)) return;
    a64_word(e, ((size == A64_SZ_D ? 2u : 0u) << 30) | (0x5u << 27) | (mode << 23)
                | (L << 22) | (((uint32_t)scaled & 0x7fu) << 15) | (rt2 << 10)
                | (rn << 5) | rt);
}
void a64_stp     (a64_emit_t *e,a64_size_t s,unsigned t,unsigned t2,unsigned n,int32_t o){ ldst_pair(e,s,2,0,t,t2,n,o); }
void a64_ldp     (a64_emit_t *e,a64_size_t s,unsigned t,unsigned t2,unsigned n,int32_t o){ ldst_pair(e,s,2,1,t,t2,n,o); }
void a64_stp_pre (a64_emit_t *e,a64_size_t s,unsigned t,unsigned t2,unsigned n,int32_t o){ ldst_pair(e,s,3,0,t,t2,n,o); }
void a64_ldp_pre (a64_emit_t *e,a64_size_t s,unsigned t,unsigned t2,unsigned n,int32_t o){ ldst_pair(e,s,3,1,t,t2,n,o); }
void a64_stp_post(a64_emit_t *e,a64_size_t s,unsigned t,unsigned t2,unsigned n,int32_t o){ ldst_pair(e,s,1,0,t,t2,n,o); }
void a64_ldp_post(a64_emit_t *e,a64_size_t s,unsigned t,unsigned t2,unsigned n,int32_t o){ ldst_pair(e,s,1,1,t,t2,n,o); }

/* ------------------------------------------------------------- branches */
static bool fits_signed(int32_t v, unsigned bits) {
    int32_t hi = ((int32_t)1 << (bits - 1u)) - 1;
    int32_t lo = -hi - 1;
    return v >= lo && v <= hi;
}
void a64_b(a64_emit_t *e, int32_t off) {
    if (bad_if(e, !fits_signed(off, 26))) return;
    a64_word(e, 0x14000000u | ((uint32_t)off & 0x03ffffffu));
}
void a64_bl(a64_emit_t *e, int32_t off) {
    if (bad_if(e, !fits_signed(off, 26))) return;
    a64_word(e, 0x94000000u | ((uint32_t)off & 0x03ffffffu));
}
void a64_bcond(a64_emit_t *e, a64_cond_t c, int32_t off) {
    if (bad_if(e, (unsigned)c > 15u || !fits_signed(off, 19))) return;
    a64_word(e, 0x54000000u | (((uint32_t)off & 0x7ffffu) << 5) | (unsigned)c);
}
static void cbz_cbnz(a64_emit_t *e, int sf, unsigned op, unsigned rt, int32_t off) {
    if (!regs_ok2(e, rt, 0)) return;
    if (bad_if(e, !fits_signed(off, 19))) return;
    a64_word(e, ((uint32_t)(sf & 1) << 31) | (0x34u << 24) | (op << 24)
                | (((uint32_t)off & 0x7ffffu) << 5) | rt);
}
void a64_cbz (a64_emit_t *e,int sf,unsigned rt,int32_t off){ cbz_cbnz(e,sf,0,rt,off); }
void a64_cbnz(a64_emit_t *e,int sf,unsigned rt,int32_t off){ cbz_cbnz(e,sf,1,rt,off); }
static void tbz_tbnz(a64_emit_t *e, unsigned op, unsigned rt, unsigned bit, int32_t off) {
    if (!regs_ok2(e, rt, 0)) return;
    if (bad_if(e, bit > 63u || !fits_signed(off, 14))) return;
    a64_word(e, ((bit >> 5) << 31) | (0x36u << 24) | (op << 24) | ((bit & 31u) << 19)
                | (((uint32_t)off & 0x3fffu) << 5) | rt);
}
void a64_tbz (a64_emit_t *e,unsigned rt,unsigned b,int32_t off){ tbz_tbnz(e,0,rt,b,off); }
void a64_tbnz(a64_emit_t *e,unsigned rt,unsigned b,int32_t off){ tbz_tbnz(e,1,rt,b,off); }

/* 1101011 | opc(4=0000 BR / 0001 BLR / 0010 RET) | 11111 | 000000 | Rn | 00000 */
static void branch_reg(a64_emit_t *e, unsigned opc, unsigned rn) {
    if (!regs_ok2(e, rn, 0)) return;
    a64_word(e, 0xd61f0000u | (opc << 21) | (rn << 5));
}
void a64_br (a64_emit_t *e, unsigned rn){ branch_reg(e, 0u, rn); }
void a64_blr(a64_emit_t *e, unsigned rn){ branch_reg(e, 1u, rn); }
void a64_ret(a64_emit_t *e, unsigned rn){ branch_reg(e, 2u, rn); }

bool a64_patch_branch(uint32_t *insn, int32_t off) {
    uint32_t w = *insn;
    if ((w & 0x7c000000u) == 0x14000000u) {              /* B / BL, imm26 */
        if (!fits_signed(off, 26)) return false;
        *insn = (w & ~0x03ffffffu) | ((uint32_t)off & 0x03ffffffu);
        return true;
    }
    if ((w & 0xff000010u) == 0x54000000u ||              /* B.cond, imm19 */
        (w & 0x7e000000u) == 0x34000000u) {              /* CBZ/CBNZ, imm19 */
        if (!fits_signed(off, 19)) return false;
        *insn = (w & ~(0x7ffffu << 5)) | (((uint32_t)off & 0x7ffffu) << 5);
        return true;
    }
    if ((w & 0x7e000000u) == 0x36000000u) {              /* TBZ/TBNZ, imm14 */
        if (!fits_signed(off, 14)) return false;
        *insn = (w & ~(0x3fffu << 5)) | (((uint32_t)off & 0x3fffu) << 5);
        return true;
    }
    return false;
}

void a64_bind(a64_emit_t *e, size_t site) {
    if (bad_if(e, site >= e->n)) return;
    if (bad_if(e, !a64_patch_branch(&e->buf[site], (int32_t)(e->n - site)))) return;
}

/* -------------------------------------------------------------- system */
/*
 * MRS: 1101 0101 0011 | o0 | op1 | CRn | CRm | op2 | Rt
 * MSR: 1101 0101 0001 | o0 | op1 | CRn | CRm | op2 | Rt      (o0 = op0 - 2)
 * mrs x0,nzcv -> 0xd53b4200 ; msr nzcv,x0 -> 0xd51b4200
 */
static void sysreg(a64_emit_t *e, uint32_t base, unsigned rt, unsigned op0,
                   unsigned op1, unsigned crn, unsigned crm, unsigned op2) {
    if (!regs_ok2(e, rt, 0)) return;
    if (bad_if(e, op0 < 2u || op0 > 3u || op1 > 7u || crn > 15u || crm > 15u || op2 > 7u)) return;
    a64_word(e, base | ((op0 - 2u) << 19) | (op1 << 16) | (crn << 12)
                | (crm << 8) | (op2 << 5) | rt);
}
void a64_mrs(a64_emit_t *e, unsigned rt, unsigned op0, unsigned op1, unsigned crn, unsigned crm, unsigned op2)
    { sysreg(e, 0xd5300000u, rt, op0, op1, crn, crm, op2); }
void a64_msr(a64_emit_t *e, unsigned rt, unsigned op0, unsigned op1, unsigned crn, unsigned crm, unsigned op2)
    { sysreg(e, 0xd5100000u, rt, op0, op1, crn, crm, op2); }
void a64_mrs_nzcv(a64_emit_t *e, unsigned rt) { a64_mrs(e, rt, 3, 3, 4, 2, 0); }
void a64_msr_nzcv(a64_emit_t *e, unsigned rt) { a64_msr(e, rt, 3, 3, 4, 2, 0); }

void a64_nop(a64_emit_t *e) { a64_word(e, 0xd503201fu); }
void a64_brk(a64_emit_t *e, uint32_t imm16) {
    if (bad_if(e, imm16 > 0xffffu)) return;
    a64_word(e, 0xd4200000u | (imm16 << 5));
}

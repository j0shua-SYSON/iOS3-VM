/*
 * iOS3-VM — ARMv6 -> arm64 dynamic recompiler, block translator.
 *
 * This is stage J2 of docs/dynarec.md: the skeleton. It exists only when the
 * project is configured with -DIOS3VM_JIT=ON, and even then it is inert until
 * something calls it. Nothing in the interpreter's boot path references it.
 *
 * THE STRUCTURAL RULE (docs/dynarec.md §2), which everything here obeys:
 *
 *     The JIT is a layer over the interpreter, never a replacement for it.
 *     Any instruction the translator does not handle ends the block *before*
 *     that instruction, and the runtime executes it with arm_step().
 *
 * So the failure mode of an unimplemented encoding is "slower", never "wrong",
 * and coverage can be grown one encoding at a time.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#ifndef IOS3VM_JIT_H
#define IOS3VM_JIT_H

#include "arm.h"
#include <stddef.h>

/* ---------------------------------------------------------- host mapping --
 * Fixed across all blocks, because chaining means one block branches into
 * another with no glue code and both must agree on where guest state lives.
 * See docs/dynarec.md §4.1 for why exactly this set.
 */
#define JIT_HOST_R0     19u   /* guest r0..r7 -> x19..x26 */
#define JIT_HOST_SP     27u   /* guest r13                */
#define JIT_HOST_CPU    28u   /* arm_cpu_t *              */
#define JIT_HOST_S0      9u   /* translator scratch       */
#define JIT_HOST_S1     10u
#define JIT_HOST_S2     11u
#define JIT_HOST_PCOUT   8u   /* resume PC at a block exit  */
#define JIT_HOST_HELPER 16u   /* IP0: helper address        */

/*
 * Frame layout of an emitted block, in bytes from the post-prologue SP:
 *   0   x29, x30          (the AAPCS64 frame record)
 *   16  x19 .. x28        (callee-saved, and every one holds guest state)
 *   96  NZCV parked across a helper call
 *   104 padding, keeping the frame 16-byte aligned
 */
#define JIT_FRAME_SIZE    112
#define JIT_FRAME_SAVED   16
#define JIT_FRAME_SCRATCH 96

/* Why a block stopped. */
typedef enum {
    JIT_END_BRANCH = 0,   /* the last instruction wrote PC                     */
    JIT_END_FALLBACK,     /* the next instruction is not translated (§2)        */
    JIT_END_FETCH_FAULT,  /* the next instruction could not be fetched          */
    JIT_END_PAGE,         /* the next instruction is in a different 4 KB page   */
    JIT_END_LIMIT,        /* the 64-instruction cap                             */
    JIT_END_CODE_FULL     /* the code buffer ran out                            */
} jit_end_t;

/* What an emitted block returns in w0. */
typedef enum {
    JIT_EXIT_NEXT = 0,    /* ran to completion; cpu->r[15] is the next PC       */
    JIT_EXIT_INTERPRET,   /* cpu->r[15] holds an instruction for arm_step()     */
    JIT_EXIT_ABORT        /* a guest access faulted; re-run cpu->r[15] in the
                           * interpreter, which re-faults identically and takes
                           * the abort. No architectural state was written, so
                           * this is exact — see the comment in jit_translate.c */
} jit_exit_t;

#define JIT_BLK_THUMB (1u << 0)
#define JIT_BLK_PRIV  (1u << 1)

/* Block key, per docs/dynarec.md §3.3. `ctx` is not populated yet (there is no
 * address-space tagging until a code cache exists) and is reserved. */
typedef struct {
    uint32_t va;
    uint32_t pa;
    uint16_t flags;
    uint16_t ctx;
} jit_key_t;

typedef struct {
    jit_key_t key;
    unsigned  insn_count;    /* guest instructions the block covers            */
    unsigned  native_count;  /* how many were emitted natively                 */
    jit_end_t end_reason;
    uint32_t *code;          /* first emitted word                             */
    size_t    code_words;
} jit_block_t;

/* Guest instruction cap per block (docs/dynarec.md §3.2 rule 6). */
#define JIT_MAX_INSNS 64

/*
 * Encoding classes that can be forced back onto the interpreter at runtime,
 * so a divergence can be bisected by feature instead of by address
 * (docs/dynarec.md §2). Denying everything must leave the boot bit-identical;
 * that is the property that makes the JIT safe to enable incrementally.
 */
#define JIT_DENY_DP       (1u << 0)
#define JIT_DENY_LDST     (1u << 1)
#define JIT_DENY_BRANCH   (1u << 2)
#define JIT_DENY_ALL      0xffffffffu
void     jit_set_deny(uint32_t mask);
uint32_t jit_get_deny(void);

/*
 * Translate one basic block starting at guest VA `va` into `code`.
 *
 * Reads guest instructions through arm_mmu_translate()/cpu->bus exactly as
 * arm_step() would, but performs no guest side effects: translation is pure.
 *
 * Returns false if not a single instruction could be translated, in which case
 * out->insn_count is 0 and the caller must interpret. A block with
 * insn_count == 0 and end_reason == JIT_END_FALLBACK is the normal "the very
 * first instruction is not one we handle" result.
 */
bool jit_translate(arm_cpu_t *cpu, uint32_t va, uint32_t *code,
                   size_t cap_words, jit_block_t *out);

/* --------------------------------------------------------- code buffers --
 * A four-function shim so the memory policy is swappable without touching the
 * translator (docs/dynarec.md §8.2): plain RWX on a jailbroken A9, MAP_JIT +
 * pthread_jit_write_protect_np on macOS and A12+, mprotect flipping elsewhere,
 * and a plain heap buffer on hosts that cannot execute arm64 at all.
 */
typedef struct {
    void  *base;
    size_t size;
    size_t used;         /* bump allocator; flush resets it (§3.7)   */
    bool   executable;   /* false on a non-arm64 host: emit and inspect only */
    bool   dual_mapped;  /* reserved for the W^X path                */
    bool   jit_protect;  /* uses pthread_jit_write_protect_np        */
} jit_buf_t;

/* True on a host where emitted arm64 code can be executed at all. */
bool jit_host_can_execute(void);

bool jit_buf_alloc(jit_buf_t *b, size_t bytes);
void jit_buf_free(jit_buf_t *b);
/* Bracket every write to the arena. On a W^X host these toggle the mapping. */
void jit_buf_begin_write(jit_buf_t *b);
void jit_buf_end_write(jit_buf_t *b);
/* Make freshly written bytes visible to the instruction stream (§8.3).
 * MUST be called for every write to the arena, including a single patched
 * branch word, before that code is executed. */
void jit_buf_commit(jit_buf_t *b, void *start, size_t len);
/* Bump-allocate `words` 32-bit words, 16-byte aligned. NULL when exhausted. */
uint32_t *jit_buf_take(jit_buf_t *b, size_t words);
/* Human-readable description of the policy actually in force. */
const char *jit_buf_policy(const jit_buf_t *b);

/*
 * Enter a translated block. Only meaningful when b->executable; on every other
 * host this returns JIT_EXIT_INTERPRET without doing anything, so callers keep
 * working (slowly) on the dev box.
 */
int jit_enter(const jit_block_t *blk, arm_cpu_t *cpu);

#endif /* IOS3VM_JIT_H */

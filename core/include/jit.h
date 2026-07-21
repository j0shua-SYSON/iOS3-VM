/*
 * iOS3-VM — ARMv6 -> arm64 dynamic recompiler, block translator.
 *
 * Stages J2 and J4 of docs/dynarec.md: the skeleton, and Thumb translation.
 * It exists only when the project is configured with -DIOS3VM_JIT=ON, and even
 * then it is inert until something calls it — there is still no code cache, so
 * nothing in the interpreter's boot path references it.
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
#define JIT_HOST_S3     12u   /* shifter carry-out, live across S0/S1 use */
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
    /* Set only by jit_block_commit(). jit_enter() binds all of this metadata
     * to the owning arena generation and current write epoch. */
    uintptr_t committed_arena;
    uintptr_t committed_code;
    uint64_t  committed_generation;
    uint64_t  committed_write_epoch;
    size_t    committed_code_words;
    unsigned  committed_insn_count;
    bool      committed;
} jit_block_t;

/* Guest instruction cap per block (docs/dynarec.md §3.2 rule 6). */
#define JIT_MAX_INSNS 64

/*
 * Encoding classes that can be forced back onto the interpreter at runtime,
 * so a divergence can be bisected by feature instead of by address
 * (docs/dynarec.md §2). Denying everything must leave the boot bit-identical;
 * that is the property that makes the JIT safe to enable incrementally.
 *
 * The three classes are instruction-set independent: a Thumb data-processing
 * instruction is denied by JIT_DENY_DP exactly as its ARM counterpart is, so
 * bisecting by feature works the same way in both states. JIT_DENY_THUMB is
 * the extra axis — it denies the whole Thumb decoder without touching ARM,
 * which is what isolates a Thumb-specific divergence in one run.
 */
#define JIT_DENY_DP       (1u << 0)
#define JIT_DENY_LDST     (1u << 1)
#define JIT_DENY_BRANCH   (1u << 2)
#define JIT_DENY_THUMB    (1u << 3)
#define JIT_DENY_ALL      0xffffffffu
void     jit_set_deny(uint32_t mask);
uint32_t jit_get_deny(void);

/*
 * Translate one basic block starting at guest VA `va` into `code`.
 *
 * Reads guest instructions through arm_mmu_translate()/cpu->bus exactly as
 * arm_step() would, but performs no guest side effects: translation is pure.
 *
 * The instruction set is taken from CPSR.T at the moment of the call and is
 * fixed for the whole block, which is why JIT_BLK_THUMB is part of the key: a
 * block never spans a change of instruction state, because every instruction
 * that can change it ends the block.
 *
 * Returns false if not a single instruction could be translated, in which case
 * out->insn_count is 0 and the caller must interpret. A block with
 * insn_count == 0 and end_reason == JIT_END_FALLBACK is the normal "the very
 * first instruction is not one we handle" result.
 */
bool jit_translate(arm_cpu_t *cpu, uint32_t va, uint32_t *code,
                   size_t cap_words, jit_block_t *out);

/* Memory-helper ABI used by emitted blocks. These are visible so the fault and
 * page-crossing contract can be tested on hosts that cannot execute AArch64:
 * loads return bit 32 on fault, stores return non-zero, and a fault is reported
 * before any bus access so interpreter replay cannot duplicate MMIO effects. */
uint64_t jit_mem_load32(arm_cpu_t *cpu, uint32_t va);
uint64_t jit_mem_load16(arm_cpu_t *cpu, uint32_t va);
uint32_t jit_mem_store32(arm_cpu_t *cpu, uint32_t va, uint32_t value);
uint32_t jit_mem_store16(arm_cpu_t *cpu, uint32_t va, uint32_t value);

/* --------------------------------------------------------- code buffers --
 * A small shim so the memory policy is swappable without touching the
 * translator (docs/dynarec.md §8.2): plain RWX on a jailbroken A9/Linux,
 * MAP_JIT + pthread_jit_write_protect_np on macOS, and a plain heap buffer on
 * hosts that cannot execute arm64 at all. The pthread toggle is unavailable on
 * iOS; if its plain-RWX policy is unavailable, allocation fails and the caller
 * keeps interpreting.
 */
typedef struct jit_buf {
    void  *base;
    size_t size;
    size_t used;         /* bump allocator; flush resets it (§3.7)   */
    uint64_t generation; /* changes whenever this object is reallocated */
    uint64_t write_epoch; /* changes on every outermost write scope      */
    bool   executable;   /* false on a non-arm64 host: emit and inspect only */
    bool   dual_mapped;  /* reserved for the W^X path                */
    bool   jit_protect;  /* macOS pthread_jit_write_protect_np policy */
    unsigned  write_depth; /* nested write scopes for this arena       */
    uintptr_t write_owner; /* macOS pthread token while depth is nonzero */
} jit_buf_t;

/* True when this build has an arm64 execution backend. Executable-memory
 * permission remains a runtime property: jit_buf_alloc() must also succeed. */
bool jit_host_can_execute(void);

/* `b` must be initially zero-initialized. After its first allocation, reuse it
 * only after jit_buf_free(): the retained generation is part of stale-block
 * rejection and must not be cleared manually. */
bool jit_buf_alloc(jit_buf_t *b, size_t bytes);
/* Returns false rather than unmapping an open macOS arena from a thread other
 * than its writer.  Freeing NULL or an already inactive arena succeeds. */
bool jit_buf_free(jit_buf_t *b);
/* Bracket every write to the arena. Scopes may be nested and may overlap
 * scopes for other arenas on the same thread. On macOS, where the hardware
 * toggle is thread-wide, the outermost scope performs the actual transition.
 * For a macOS MAP_JIT arena, a scope must end (or be abandoned by free) on
 * the thread that opened it. These calls are not a substitute for the code
 * cache's external lock. */
bool jit_buf_begin_write(jit_buf_t *b);
bool jit_buf_end_write(jit_buf_t *b);
/* Make freshly written bytes visible to the instruction stream (§8.3).
 * MUST be called for every write to the arena, including a single patched
 * branch word, before that code is executed. Returns false unless at least one
 * write scope completed, or for an empty range/range not wholly contained in
 * the bump-allocated portion of the arena. */
bool jit_buf_commit(jit_buf_t *b, void *start, size_t len);
/* Commit one translated block and bind it to this allocation generation and
 * write epoch. Opening any later outermost write scope conservatively
 * invalidates every committed block in the arena until it is recommitted. */
bool jit_block_commit(jit_buf_t *b, jit_block_t *blk);
/* Bump-allocate `words` 32-bit words, 16-byte aligned. NULL when exhausted. */
uint32_t *jit_buf_take(jit_buf_t *b, size_t words);
/* Human-readable description of the policy actually in force. */
const char *jit_buf_policy(const jit_buf_t *b);

/*
 * Enter a translated block. The block must have been committed through
 * jit_block_commit() against `arena`; range, generation, write-epoch and
 * closed-write-state checks fail safely to JIT_EXIT_INTERPRET. On a non-arm64
 * host this always returns JIT_EXIT_INTERPRET, so callers keep working slowly.
 */
int jit_enter(const jit_buf_t *arena, const jit_block_t *blk, arm_cpu_t *cpu);

#endif /* IOS3VM_JIT_H */

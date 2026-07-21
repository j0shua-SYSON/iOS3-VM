/*
 * iOS3-VM — JIT code buffers, and the one place that enters emitted code.
 *
 * Four functions (alloc / begin_write / end_write / commit) are the entire
 * platform surface of the dynarec, so a change of host memory policy costs
 * this file and nothing else (docs/dynarec.md §8.2). Three policies are
 * anticipated and two are implemented here:
 *
 *   plain RWX          the intended target. The A9 predates APRR, PAC and PPL,
 *                      so on a jailbroken device with CS_DEBUGGED set an
 *                      ordinary mmap(PROT_EXEC) is executable and neither
 *                      begin_write nor end_write has to do anything.
 *   MAP_JIT + W^X      the macOS-on-Apple-Silicon policy: the mapping is either
 *                      writable or executable per thread, flipped by
 *                      pthread_jit_write_protect_np(). Apple does not expose
 *                      that API on iOS, so iOS must never advertise or call it.
 *   dual mapping       one memory object mapped RW at one address and RX at
 *                      another (mach_make_memory_entry_64 + mach_vm_remap).
 *                      NOT implemented, and deliberately so: on iOS the
 *                      obstacle is code-signing enforcement of unsigned
 *                      executable pages, not the simultaneity of W and X, so
 *                      dual mapping does not remove the jailbreak requirement.
 *                      The b->dual_mapped field is where it would announce
 *                      itself.
 *
 * On a host that cannot execute arm64 at all — the project's Windows/x86 dev
 * box — the arena is ordinary heap memory and `executable` is false. The
 * translator still runs and its output can still be inspected byte for byte,
 * which is what core/tests/test_jit.c does.
 *
 * Copyright (c) 2026 j0shua-SYSON. MIT licensed.
 */
#include "jit.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#if (defined(__aarch64__) || defined(__arm64__)) && (defined(__APPLE__) || defined(__linux__))
#  define JIT_HOST_EXEC 1
#  include <sys/mman.h>
#  if defined(__APPLE__)
#    include <libkern/OSCacheControl.h>
#    include <TargetConditionals.h>
#    if defined(MAP_JIT) && defined(TARGET_OS_OSX) && TARGET_OS_OSX && \
        defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
#      define JIT_APPLE_THREAD_WX 1
#      include <pthread.h>
#    endif
#  endif
#else
#  define JIT_HOST_EXEC 0
#endif

#ifndef JIT_APPLE_THREAD_WX
#  define JIT_APPLE_THREAD_WX 0
#endif

#if JIT_APPLE_THREAD_WX
/* pthread_jit_write_protect_np() changes every MAP_JIT mapping for the current
 * thread. A per-buffer Boolean cannot represent nested or overlapping write
 * scopes, so keep the hardware transition depth in thread-local storage and
 * keep only ownership/nesting information in each arena. */
static _Thread_local unsigned g_jit_thread_write_depth;

static uintptr_t jit_current_thread(void) {
    return (uintptr_t)(void *)pthread_self();
}
#endif

/* iOS/macOS arm64 pages are 16 KB; align the arena so any future mprotect- or
 * remap-based policy can operate on it without a copy (docs/dynarec.md §8.4). */
#define JIT_ARENA_ALIGN 0x4000u

_Static_assert((JIT_ARENA_ALIGN & (JIT_ARENA_ALIGN - 1u)) == 0,
               "JIT arena alignment must be a power of two");

bool jit_host_can_execute(void) { return JIT_HOST_EXEC ? true : false; }

static bool jit_buf_is_inactive(const jit_buf_t *b) {
    return b && !b->base && b->size == 0 && b->used == 0 &&
           b->write_epoch == 0 &&
           !b->executable && !b->dual_mapped && !b->jit_protect &&
           b->write_depth == 0 && b->write_owner == 0;
}

static void jit_buf_reset_inactive(jit_buf_t *b) {
    uint64_t generation = b->generation;
    memset(b, 0, sizeof *b);
    b->generation = generation;
}

static bool jit_buf_range_valid(const jit_buf_t *b, const void *start,
                                size_t len) {
    uintptr_t base, addr, delta;

    if (!b || !b->base || !start || len == 0 || b->used > b->size)
        return false;
    base = (uintptr_t)b->base;
    addr = (uintptr_t)start;
    if (addr < base) return false;
    delta = addr - base;
    if (delta > (uintptr_t)b->used) return false;
    return len <= b->used - (size_t)delta;
}

bool jit_buf_alloc(jit_buf_t *b, size_t bytes) {
    uint64_t next_generation;

    if (!jit_buf_is_inactive(b)) return false;
    if (bytes == 0) return false;
    if (bytes > SIZE_MAX - (JIT_ARENA_ALIGN - 1u)) return false;
    bytes = (bytes + (JIT_ARENA_ALIGN - 1u)) & ~(size_t)(JIT_ARENA_ALIGN - 1u);
    /* Saturating is safer than making an ancient generation valid again.
     * This can only be reached after 2^64-1 allocations of the same object. */
    if (b->generation == UINT64_MAX) return false;
    next_generation = b->generation + 1u;

#if JIT_HOST_EXEC
    {
        void *p = MAP_FAILED;
#  if JIT_APPLE_THREAD_WX
        p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (p == NULL) {
            (void)munmap(p, bytes);
            p = MAP_FAILED;
        }
        if (p != MAP_FAILED) b->jit_protect = true;
#  endif
        if (p == MAP_FAILED) {
            p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
            b->jit_protect = false;
        }
        /* NULL is a valid mmap address on a few permissive kernels, but this
         * API reserves it as the inactive sentinel. Do not leak such a map. */
        if (p == NULL) {
            (void)munmap(p, bytes);
            p = MAP_FAILED;
        }
        if (p == MAP_FAILED) {
            b->jit_protect = false;
            return false;
        }
        b->base = p;
        b->size = bytes;
        b->generation = next_generation;
        b->executable = true;
        return true;
    }
#else
    b->base = malloc(bytes);
    if (!b->base) return false;
    b->size = bytes;
    b->generation = next_generation;
    b->executable = false;
    return true;
#endif
}

bool jit_buf_free(jit_buf_t *b) {
    if (!b) return true;
#if JIT_APPLE_THREAD_WX
    if (b->base && b->jit_protect && b->write_depth != 0) {
        if (b->write_owner != jit_current_thread() ||
            g_jit_thread_write_depth < b->write_depth)
            return false;
        g_jit_thread_write_depth -= b->write_depth;
        if (g_jit_thread_write_depth == 0)
            pthread_jit_write_protect_np(1);
    }
#endif
    if (!b->base) {
        jit_buf_reset_inactive(b);
        return true;
    }
    b->write_depth = 0;
    b->write_owner = 0;
#if JIT_HOST_EXEC
    if (munmap(b->base, b->size) != 0) return false;
#else
    free(b->base);
#endif
    jit_buf_reset_inactive(b);
    return true;
}

bool jit_buf_begin_write(jit_buf_t *b) {
    if (!b || !b->base || b->write_depth == UINT_MAX) return false;
    /* A later write invalidates every existing block in this arena. Refuse the
     * physically unreachable wrap rather than reviving epoch 1 blocks. A
     * free/reallocation resets the epoch under a new allocation generation. */
    if (b->write_depth == 0 && b->write_epoch == UINT64_MAX) return false;
#if JIT_APPLE_THREAD_WX
    if (b->jit_protect) {
        uintptr_t self = jit_current_thread();
        if (b->write_depth != 0 && b->write_owner != self) return false;
        if (g_jit_thread_write_depth == UINT_MAX) return false;
        if (b->write_depth == 0) b->write_owner = self;
        if (g_jit_thread_write_depth == 0)
            pthread_jit_write_protect_np(0);
        g_jit_thread_write_depth++;
    }
#endif
    if (b->write_depth == 0) b->write_epoch++;
    b->write_depth++;
    return true;
}

bool jit_buf_end_write(jit_buf_t *b) {
    if (!b || !b->base || b->write_depth == 0) return false;
#if JIT_APPLE_THREAD_WX
    if (b->jit_protect) {
        if (b->write_owner != jit_current_thread() ||
            g_jit_thread_write_depth < b->write_depth)
            return false;
        g_jit_thread_write_depth--;
        if (g_jit_thread_write_depth == 0)
            pthread_jit_write_protect_np(1);
    }
#endif
    b->write_depth--;
    if (b->write_depth == 0) b->write_owner = 0;
    return true;
}

/*
 * arm64's I-cache is not coherent with the D-cache, and getting this wrong
 * produces stale-code bugs that are non-deterministic and unreproducible under
 * a debugger. Do not hand-roll the dc cvau / dsb / ic ivau / dsb / isb
 * sequence: the platform primitives below perform exactly it, using the line
 * size from CTR_EL0 rather than assuming 64 bytes (docs/dynarec.md §8.3).
 *
 * This must be called for EVERY write to the arena, including a single patched
 * branch word when chaining lands.
 */
bool jit_buf_commit(jit_buf_t *b, void *start, size_t len) {
    if (!jit_buf_range_valid(b, start, len) || b->write_epoch == 0 ||
        b->write_depth != 0)
        return false;
#if JIT_APPLE_THREAD_WX
    /* A different MAP_JIT arena may still have this thread globally writable. */
    if (g_jit_thread_write_depth != 0) return false;
#endif

#if JIT_HOST_EXEC
#  if defined(__APPLE__)
    sys_icache_invalidate(start, len);
#  else
    __builtin___clear_cache((char *)start, (char *)start + len);
#  endif
#else
    (void)start; (void)len;
#endif
    return true;
}

bool jit_block_commit(jit_buf_t *b, jit_block_t *blk) {
    size_t bytes;

    if (blk) {
        blk->committed = false;
        blk->committed_arena = 0;
        blk->committed_code = 0;
        blk->committed_generation = 0;
        blk->committed_write_epoch = 0;
        blk->committed_code_words = 0;
        blk->committed_insn_count = 0;
    }
    if (!b || b->generation == 0 || !blk || !blk->code ||
        ((uintptr_t)blk->code & 3u) != 0 ||
        blk->insn_count == 0 ||
        blk->code_words == 0 || blk->code_words > SIZE_MAX / sizeof(uint32_t))
        return false;
    bytes = blk->code_words * sizeof(uint32_t);
    if (!jit_buf_commit(b, blk->code, bytes)) return false;
    blk->committed_arena = (uintptr_t)(void *)b;
    blk->committed_code = (uintptr_t)(void *)blk->code;
    blk->committed_generation = b->generation;
    blk->committed_write_epoch = b->write_epoch;
    blk->committed_code_words = blk->code_words;
    blk->committed_insn_count = blk->insn_count;
    blk->committed = true;
    return true;
}

uint32_t *jit_buf_take(jit_buf_t *b, size_t words) {
    size_t off, bytes, pad;
    if (!b || !b->base || words == 0) return NULL;
    if (words > SIZE_MAX / sizeof(uint32_t)) return NULL;
    if (b->used > b->size) return NULL;
    bytes = words * sizeof(uint32_t);

    /* Align the returned address, not merely its offset: malloc() need not
     * provide 16-byte alignment on every non-executing inspection host. The
     * low-bit calculation cannot overflow even for an adversarial used value. */
    pad = (size_t)(-(uintptr_t)((uintptr_t)b->base + b->used)) & 15u;
    if (pad > b->size - b->used) return NULL;
    off = b->used + pad;
    if (off > b->size || bytes > b->size - off) return NULL;
    b->used = off + bytes;
    return (uint32_t *)(void *)((char *)b->base + off);
}

const char *jit_buf_policy(const jit_buf_t *b) {
    if (!b || !b->base)   return "unallocated";
    if (!b->executable)   return "heap (not executable: emit and inspect only)";
    if (b->dual_mapped)   return "dual-mapped RW/RX";
    if (b->jit_protect)   return "MAP_JIT + pthread_jit_write_protect_np";
    return "plain RWX";
}

/*
 * The single place emitted code is entered. A block is an ordinary AAPCS64
 * function `int (*)(arm_cpu_t *)` returning a jit_exit_t, so there is no
 * hand-written trampoline to get wrong.
 */
int jit_enter(const jit_buf_t *arena, const jit_block_t *blk, arm_cpu_t *cpu) {
    size_t bytes;

    if (!arena || !blk || !cpu || !arena->base || arena->generation == 0 ||
        arena->write_epoch == 0 ||
        !arena->executable ||
        arena->write_depth != 0 || !blk->code ||
        ((uintptr_t)blk->code & 3u) != 0 || blk->insn_count == 0 ||
        blk->code_words == 0 || !blk->committed ||
        blk->committed_arena != (uintptr_t)(const void *)arena ||
        blk->committed_code != (uintptr_t)(const void *)blk->code ||
        blk->committed_generation != arena->generation ||
        blk->committed_write_epoch != arena->write_epoch ||
        blk->committed_code_words != blk->code_words ||
        blk->committed_insn_count != blk->insn_count ||
        blk->code_words > SIZE_MAX / sizeof(uint32_t))
        return JIT_EXIT_INTERPRET;
#if JIT_APPLE_THREAD_WX
    if (g_jit_thread_write_depth != 0) return JIT_EXIT_INTERPRET;
#endif
    bytes = blk->code_words * sizeof(uint32_t);
    if (!jit_buf_range_valid(arena, blk->code, bytes))
        return JIT_EXIT_INTERPRET;
#if JIT_HOST_EXEC
    union { void *p; int (*fn)(arm_cpu_t *); } u;
    u.p = (void *)blk->code;
    return u.fn(cpu);
#else
    /* Nothing to do on a host that cannot execute arm64: the caller falls back
     * to arm_step(), which is the correct answer, just slower. */
    (void)bytes;
    return JIT_EXIT_INTERPRET;
#endif
}

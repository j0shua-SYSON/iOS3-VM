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
 *   MAP_JIT + W^X      what macOS on Apple Silicon and any A12+ iOS host
 *                      requires: the mapping is either writable or executable
 *                      per thread, flipped by pthread_jit_write_protect_np().
 *                      This is also what the arm64 CI runner needs.
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
#include <stdlib.h>
#include <string.h>

#if (defined(__aarch64__) || defined(__arm64__)) && (defined(__APPLE__) || defined(__linux__))
#  define JIT_HOST_EXEC 1
#  include <sys/mman.h>
#  if defined(__APPLE__)
#    include <libkern/OSCacheControl.h>
#    include <pthread.h>
#  endif
#else
#  define JIT_HOST_EXEC 0
#endif

/* iOS/macOS arm64 pages are 16 KB; align the arena so any future mprotect- or
 * remap-based policy can operate on it without a copy (docs/dynarec.md §8.4). */
#define JIT_ARENA_ALIGN 0x4000u

bool jit_host_can_execute(void) { return JIT_HOST_EXEC ? true : false; }

bool jit_buf_alloc(jit_buf_t *b, size_t bytes) {
    memset(b, 0, sizeof *b);
    if (bytes == 0) return false;
    bytes = (bytes + (JIT_ARENA_ALIGN - 1u)) & ~(size_t)(JIT_ARENA_ALIGN - 1u);

#if JIT_HOST_EXEC
    {
        void *p = MAP_FAILED;
#  if defined(__APPLE__) && defined(MAP_JIT)
        p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (p != MAP_FAILED) b->jit_protect = true;
#  endif
        if (p == MAP_FAILED) {
            p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
            b->jit_protect = false;
        }
        if (p == MAP_FAILED) return false;
        b->base = p;
        b->size = bytes;
        b->executable = true;
        return true;
    }
#else
    b->base = malloc(bytes);
    if (!b->base) return false;
    b->size = bytes;
    b->executable = false;
    return true;
#endif
}

void jit_buf_free(jit_buf_t *b) {
    if (!b || !b->base) return;
#if JIT_HOST_EXEC
    munmap(b->base, b->size);
#else
    free(b->base);
#endif
    memset(b, 0, sizeof *b);
}

void jit_buf_begin_write(jit_buf_t *b) {
#if JIT_HOST_EXEC && defined(__APPLE__)
    if (b && b->jit_protect) pthread_jit_write_protect_np(0);
#else
    (void)b;
#endif
}

void jit_buf_end_write(jit_buf_t *b) {
#if JIT_HOST_EXEC && defined(__APPLE__)
    if (b && b->jit_protect) pthread_jit_write_protect_np(1);
#else
    (void)b;
#endif
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
void jit_buf_commit(jit_buf_t *b, void *start, size_t len) {
    (void)b;
#if JIT_HOST_EXEC
#  if defined(__APPLE__)
    sys_icache_invalidate(start, len);
#  else
    __builtin___clear_cache((char *)start, (char *)start + len);
#  endif
#else
    (void)start; (void)len;
#endif
}

uint32_t *jit_buf_take(jit_buf_t *b, size_t words) {
    size_t off, bytes = words * 4u;
    if (!b || !b->base || words == 0) return NULL;
    off = (b->used + 15u) & ~(size_t)15u;
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
int jit_enter(const jit_block_t *blk, arm_cpu_t *cpu) {
#if JIT_HOST_EXEC
    union { void *p; int (*fn)(arm_cpu_t *); } u;
    if (!blk || !blk->code || blk->insn_count == 0) return JIT_EXIT_INTERPRET;
    u.p = (void *)blk->code;
    return u.fn(cpu);
#else
    /* Nothing to do on a host that cannot execute arm64: the caller falls back
     * to arm_step(), which is the correct answer, just slower. */
    (void)blk; (void)cpu;
    return JIT_EXIT_INTERPRET;
#endif
}

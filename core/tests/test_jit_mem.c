/* Focused, allocation-light boundary tests for the JIT host-memory shim. */
#include "jit.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(__APPLE__)
#  include <TargetConditionals.h>
#  if defined(TARGET_OS_OSX) && TARGET_OS_OSX && \
      defined(TARGET_OS_IPHONE) && !TARGET_OS_IPHONE
#    define TEST_JIT_APPLE_THREAD_WX 1
#    include <pthread.h>
#  endif
#endif
#ifndef TEST_JIT_APPLE_THREAD_WX
#  define TEST_JIT_APPLE_THREAD_WX 0
#endif

static int g_pass, g_fail;

#define CHECK(cond, ...) do {                                                \
    if (cond) { g_pass++; }                                                  \
    else {                                                                  \
        g_fail++;                                                            \
        printf("  FAIL %s:%d: ", __func__, __LINE__);                       \
        printf(__VA_ARGS__);                                                 \
        printf("\n");                                                       \
    }                                                                       \
} while (0)

/* generation intentionally survives free so stale blocks cannot become valid
 * merely because a later mapping reused the same address. */
static bool is_inactive(const jit_buf_t *b) {
    return b->base == NULL && b->size == 0 && b->used == 0 &&
           b->write_epoch == 0 &&
           !b->executable && !b->dual_mapped && !b->jit_protect &&
           b->write_depth == 0 && b->write_owner == 0;
}

static void test_bad_allocations(void) {
    jit_buf_t b = {0};
    jit_buf_t exhausted = {0};

    CHECK(!jit_buf_alloc(NULL, 1), "NULL output pointer accepted");
    CHECK(!jit_buf_alloc(&b, 0), "zero-byte arena accepted");
    CHECK(is_inactive(&b), "zero-byte failure changed buffer state");
    CHECK(!jit_buf_alloc(&b, SIZE_MAX), "alignment-overflowing arena accepted");
    CHECK(is_inactive(&b), "overflow failure changed buffer state");
    exhausted.generation = UINT64_MAX;
    CHECK(!jit_buf_alloc(&exhausted, 1), "wrapped a saturated generation");
    CHECK(is_inactive(&exhausted),
          "saturated-generation failure changed buffer state");

    if (!jit_buf_alloc(&b, 1)) {
        printf("  SKIP: this host refused an arena for reallocation checks\n");
        return;
    }
    {
        void *base = b.base;
        uint64_t generation = b.generation;
        CHECK(!jit_buf_alloc(&b, 1), "active arena was silently overwritten");
        CHECK(b.base == base && b.generation == generation,
              "failed reallocation changed the live arena");
        CHECK(jit_buf_free(&b), "could not free reallocation-test arena");
        CHECK(is_inactive(&b), "free did not leave an inactive arena");
        if (jit_buf_alloc(&b, 1)) {
            CHECK(b.generation != generation,
                  "reallocation reused generation %llu",
                  (unsigned long long)generation);
            CHECK(jit_buf_free(&b), "could not free reallocated arena");
        } else {
            printf("  SKIP: this host refused a second arena allocation\n");
        }
    }
}

static void test_take_and_commit_bounds(void) {
    jit_buf_t b = {0};
    uint32_t *first, *second;
    size_t saved_used;

    if (!jit_buf_alloc(&b, 1)) {
        printf("  SKIP: this host refused the smallest JIT arena\n");
        return;
    }

    CHECK(b.size >= 1 && (b.size & 0x3fffu) == 0,
          "arena size %zu is not rounded to 16 KB", b.size);
    CHECK(jit_buf_take(&b, 0) == NULL, "zero-word allocation accepted");
    CHECK(b.used == 0, "zero-word allocation advanced used to %zu", b.used);
    CHECK(jit_buf_take(&b, SIZE_MAX / sizeof(uint32_t) + 1u) == NULL,
          "words-to-bytes overflow accepted");
    CHECK(b.used == 0, "overflowing word request advanced used to %zu", b.used);

    first = jit_buf_take(&b, 1);
    second = jit_buf_take(&b, 1);
    CHECK(first != NULL && second != NULL, "small allocations failed");
    CHECK(first && (((uintptr_t)first & 15u) == 0), "first allocation is unaligned");
    CHECK(second && (((uintptr_t)second & 15u) == 0), "second allocation is unaligned");

    if (first && second) {
        uint64_t saved_epoch;
        CHECK(!jit_buf_commit(&b, first, sizeof *first),
              "commit accepted before any write scope");
        if (!jit_buf_begin_write(&b)) {
            CHECK(false, "could not open write scope");
        } else {
            first[0] = 0xd503201fu;
            second[0] = 0xd503201fu;
            CHECK(!jit_buf_commit(&b, first, sizeof *first),
                  "commit accepted while the arena was writable");
            CHECK(jit_buf_end_write(&b), "could not close write scope");
        }

        CHECK(jit_buf_commit(&b, first, sizeof *first), "valid commit refused");
        saved_epoch = b.write_epoch;
        b.write_epoch = UINT64_MAX;
        CHECK(!jit_buf_begin_write(&b), "wrapped a saturated write epoch");
        CHECK(b.write_depth == 0, "failed saturated begin changed depth");
        b.write_epoch = saved_epoch;
        CHECK(!jit_buf_commit(NULL, first, sizeof *first), "NULL arena accepted");
        CHECK(!jit_buf_commit(&b, NULL, sizeof *first), "NULL range accepted");
        CHECK(!jit_buf_commit(&b, first, 0), "empty commit accepted");
        CHECK(!jit_buf_commit(&b, first, SIZE_MAX), "overflowing length accepted");
        CHECK(!jit_buf_commit(&b, (uint8_t *)b.base + b.used, 1),
              "unallocated arena tail accepted");
        CHECK(!jit_buf_commit(&b, first, b.used + 1u),
              "range crossing the allocated frontier accepted");
        CHECK(!jit_buf_commit(&b, (uint8_t *)b.base + b.size, 1),
              "range beyond the arena accepted");
    }

    saved_used = b.used;
    b.used = SIZE_MAX - 7u;
    CHECK(jit_buf_take(&b, 1) == NULL, "corrupt huge used value accepted");
    CHECK(!first || !jit_buf_commit(&b, first, sizeof *first),
          "commit accepted a corrupt huge used value");
    CHECK(b.used == SIZE_MAX - 7u, "failed request changed corrupt used value");
    b.used = b.size + 1u;
    CHECK(jit_buf_take(&b, 1) == NULL, "used value past arena accepted");
    CHECK(!first || !jit_buf_commit(&b, first, sizeof *first),
          "commit accepted used past the arena");
    b.used = saved_used;

    CHECK(jit_buf_free(&b), "free failed");
    CHECK(is_inactive(&b), "free did not clear active buffer state");
    CHECK(jit_buf_free(&b), "second free failed");
    CHECK(is_inactive(&b), "second free changed inactive buffer state");
}

static void test_nested_scopes_and_free_cleanup(void) {
    jit_buf_t b = {0};
    uint32_t *word;
    uint64_t first_epoch;

    if (!jit_buf_alloc(&b, 64)) {
        printf("  SKIP: this host refused an arena for nesting checks\n");
        return;
    }
    word = jit_buf_take(&b, 1);
    if (!word) {
        CHECK(false, "nested-scope word allocation failed");
        CHECK(jit_buf_free(&b), "cleanup after word-allocation failure");
        return;
    }

    if (!jit_buf_begin_write(&b)) {
        CHECK(false, "first begin failed");
        CHECK(jit_buf_free(&b), "cleanup after first begin failure");
        return;
    }
    first_epoch = b.write_epoch;
    CHECK(first_epoch != 0, "first outer write scope kept epoch zero");
    if (!jit_buf_begin_write(&b)) {
        CHECK(false, "nested begin failed");
        CHECK(jit_buf_free(&b), "cleanup after nested begin failure");
        return;
    }
    CHECK(b.write_epoch == first_epoch,
          "nested begin changed write epoch from %llu to %llu",
          (unsigned long long)first_epoch,
          (unsigned long long)b.write_epoch);
    CHECK(b.write_depth == 2, "nested depth=%u expect 2", b.write_depth);
    word[0] = 0xd503201fu;
    CHECK(!jit_buf_commit(&b, word, sizeof *word),
          "commit accepted a nested open scope");
    CHECK(jit_buf_end_write(&b), "first nested end failed");
    CHECK(b.write_depth == 1, "depth after one end=%u expect 1", b.write_depth);
    CHECK(!jit_buf_commit(&b, word, sizeof *word),
          "commit accepted the still-open outer scope");

    /* free is the error-path escape hatch: on macOS it must close every scope
     * owned by this thread before unmapping, without disturbing other arenas. */
    CHECK(jit_buf_free(&b), "free could not abandon a same-thread write scope");
    CHECK(is_inactive(&b), "free left nested write state behind");
    CHECK(!jit_buf_end_write(&b), "end accepted an inactive arena");

    /* A new write after cleanup catches a thread that was accidentally left RX
     * or writable by the abandoned nested scope. */
    if (jit_buf_alloc(&b, 64)) {
        word = jit_buf_take(&b, 1);
        CHECK(word != NULL, "post-cleanup word allocation failed");
        if (word) {
            if (!jit_buf_begin_write(&b)) {
                CHECK(false, "post-cleanup begin failed");
            } else {
                word[0] = 0xd503201fu;
                CHECK(jit_buf_end_write(&b), "post-cleanup end failed");
                CHECK(jit_buf_commit(&b, word, sizeof *word),
                      "post-cleanup commit failed");
            }
        }
        CHECK(jit_buf_free(&b), "post-cleanup free failed");
    } else {
        printf("  SKIP: this host refused the post-cleanup arena\n");
    }
}

static void test_overlapping_arenas(void) {
    jit_buf_t a = {0}, b = {0};
    uint32_t *aw, *bw;
    bool a_open = false, b_open = false;

    if (!jit_buf_alloc(&a, 64) || !jit_buf_alloc(&b, 64)) {
        printf("  SKIP: this host refused two simultaneous JIT arenas\n");
        CHECK(jit_buf_free(&a), "cleanup first overlapping arena");
        CHECK(jit_buf_free(&b), "cleanup second overlapping arena");
        return;
    }
    aw = jit_buf_take(&a, 1);
    bw = jit_buf_take(&b, 1);
    if (!aw || !bw) {
        CHECK(false, "overlapping-arena word allocation failed");
        CHECK(jit_buf_free(&a), "cleanup first failed arena");
        CHECK(jit_buf_free(&b), "cleanup second failed arena");
        return;
    }

    a_open = jit_buf_begin_write(&a);
    b_open = jit_buf_begin_write(&b);
    CHECK(a_open && b_open, "overlapping begin failed");
    if (a_open && b_open) {
        bool closed;
        aw[0] = bw[0] = 0xd503201fu;
        closed = jit_buf_end_write(&b);
        CHECK(closed, "closing second arena failed");
        if (closed) b_open = false;
#if TEST_JIT_APPLE_THREAD_WX
        if (closed && a.jit_protect) {
            CHECK(!jit_buf_commit(&b, bw, sizeof *bw),
                  "commit ignored another open MAP_JIT scope on this thread");
        }
#endif
        closed = jit_buf_end_write(&a);
        CHECK(closed, "closing first arena failed");
        if (closed) a_open = false;
        if (!a_open && !b_open) {
            CHECK(jit_buf_commit(&a, aw, sizeof *aw), "first arena commit failed");
            CHECK(jit_buf_commit(&b, bw, sizeof *bw), "second arena commit failed");
        }
    }
    if (b_open) CHECK(jit_buf_end_write(&b), "cleanup second write scope failed");
    if (a_open) CHECK(jit_buf_end_write(&a), "cleanup first write scope failed");
    CHECK(jit_buf_free(&a), "free first overlapping arena failed");
    CHECK(jit_buf_free(&b), "free second overlapping arena failed");
}

#if TEST_JIT_APPLE_THREAD_WX
typedef struct {
    jit_buf_t *buf;
    bool end_result;
    bool free_result;
} wrong_thread_ctx_t;

static void *wrong_thread_close(void *opaque) {
    wrong_thread_ctx_t *ctx = (wrong_thread_ctx_t *)opaque;
    ctx->end_result = jit_buf_end_write(ctx->buf);
    ctx->free_result = jit_buf_free(ctx->buf);
    return NULL;
}

static void test_wrong_thread_is_rejected(void) {
    jit_buf_t b = {0};
    wrong_thread_ctx_t ctx;
    pthread_t thread;

    if (!jit_buf_alloc(&b, 64)) {
        printf("  SKIP: this host refused an arena for ownership checks\n");
        return;
    }
    if (!b.jit_protect) {
        printf("  SKIP: this macOS host did not select MAP_JIT W^X\n");
        CHECK(jit_buf_free(&b), "free non-MAP_JIT ownership arena");
        return;
    }
    if (!jit_buf_begin_write(&b)) {
        CHECK(false, "ownership-test begin failed");
        CHECK(jit_buf_free(&b), "ownership-test cleanup failed");
        return;
    }
    memset(&ctx, 0, sizeof ctx);
    ctx.buf = &b;
    if (pthread_create(&thread, NULL, wrong_thread_close, &ctx) != 0) {
        CHECK(false, "pthread_create failed");
        CHECK(jit_buf_end_write(&b), "ownership-test local end failed");
        CHECK(jit_buf_free(&b), "ownership-test local free failed");
        return;
    }
    if (pthread_join(thread, NULL) != 0) {
        CHECK(false, "pthread_join failed");
        return;
    }
    CHECK(!ctx.end_result, "wrong thread closed the write scope");
    CHECK(!ctx.free_result, "wrong thread unmapped an open arena");
    CHECK(b.write_depth == 1, "wrong thread changed depth to %u", b.write_depth);
    CHECK(jit_buf_end_write(&b), "owner could not close its write scope");
    CHECK(jit_buf_free(&b), "owner could not free its arena");
}
#endif

static void test_block_entry_guards(void) {
    jit_buf_t b = {0}, wrong = {0};
    jit_block_t blk;
    arm_cpu_t cpu;
    uint32_t *code, *other;
    uint64_t generation;
    uint64_t committed_epoch;
    uintptr_t committed_code;
    size_t committed_words;
    unsigned committed_insns;

    if (!jit_buf_alloc(&b, 64)) {
        printf("  SKIP: this host refused an arena for entry checks\n");
        return;
    }
    code = jit_buf_take(&b, 1);
    other = jit_buf_take(&b, 1);
    if (!code || !other || !jit_buf_begin_write(&b)) {
        CHECK(false, "could not prepare entry-guard code");
        CHECK(jit_buf_free(&b), "entry-guard setup cleanup failed");
        return;
    }
    code[0] = 0xd65f03c0u; /* ret */
    other[0] = 0xd65f03c0u;
    CHECK(jit_buf_end_write(&b), "entry-guard end failed");
    CHECK(jit_buf_commit(&b, other, sizeof *other),
          "safe alternate entry-guard code commit failed");

    memset(&blk, 0, sizeof blk);
    memset(&cpu, 0, sizeof cpu);
    blk.code = code;
    blk.code_words = 1;
    blk.insn_count = 1;
    CHECK(jit_enter(&b, &blk, &cpu) == JIT_EXIT_INTERPRET,
          "uncommitted block was entered");
    CHECK(jit_block_commit(&b, &blk), "block commit failed");
    CHECK(jit_enter(&b, &blk, NULL) == JIT_EXIT_INTERPRET,
          "NULL CPU was accepted");

    blk.code = other;
    CHECK(jit_enter(&b, &blk, &cpu) == JIT_EXIT_INTERPRET,
          "post-commit code-pointer corruption was accepted");
    blk.code = code;

    committed_words = blk.code_words;
    blk.code_words = 0;
    CHECK(jit_enter(&b, &blk, &cpu) == JIT_EXIT_INTERPRET,
          "zero-word block was entered");
    blk.code_words = committed_words + 1u;
    CHECK(jit_enter(&b, &blk, &cpu) == JIT_EXIT_INTERPRET,
          "post-commit code-size corruption was accepted");
    blk.code_words = committed_words;
    committed_insns = blk.insn_count;
    blk.insn_count++;
    CHECK(jit_enter(&b, &blk, &cpu) == JIT_EXIT_INTERPRET,
          "post-commit instruction-count corruption was accepted");
    blk.insn_count = committed_insns;
    CHECK(jit_enter(&wrong, &blk, &cpu) == JIT_EXIT_INTERPRET,
          "wrong arena was accepted");

    committed_epoch = blk.committed_write_epoch;
    if (!jit_buf_begin_write(&b)) {
        CHECK(false, "entry-guard reopen failed");
    } else {
        CHECK(jit_enter(&b, &blk, &cpu) == JIT_EXIT_INTERPRET,
              "block entered while its arena was writable");
        code[0] = 0xd65f03c0u; /* even an identical rewrite is conservatively dirty */
        CHECK(jit_buf_end_write(&b), "entry-guard reclose failed");
        CHECK(b.write_epoch != committed_epoch,
              "outermost write did not advance the arena epoch");
        CHECK(jit_enter(&b, &blk, &cpu) == JIT_EXIT_INTERPRET,
              "mutated block was entered without recommit");
        CHECK(jit_block_commit(&b, &blk),
              "recommit after arena mutation failed");
        CHECK(blk.committed_write_epoch == b.write_epoch,
              "recommit captured stale write epoch");
    }

    generation = blk.committed_generation;
    blk.committed_generation++;
    CHECK(jit_enter(&b, &blk, &cpu) == JIT_EXIT_INTERPRET,
          "stale allocation generation was accepted");
    blk.committed_generation = generation;
    committed_code = blk.committed_code;
    blk.code = (uint32_t *)(void *)((uint8_t *)b.base + b.used);
    blk.committed_code = (uintptr_t)(void *)blk.code;
    CHECK(jit_enter(&b, &blk, &cpu) == JIT_EXIT_INTERPRET,
          "block outside the allocated frontier was accepted");
    blk.code = code;
    blk.committed_code = committed_code;

    CHECK(jit_buf_free(&b), "entry-guard free failed");
    CHECK(jit_enter(&b, &blk, &cpu) == JIT_EXIT_INTERPRET,
          "block from a freed arena was entered");
    if (jit_buf_alloc(&b, 64)) {
        CHECK(b.generation != generation,
              "reallocated arena reused stale generation");
        CHECK(jit_enter(&b, &blk, &cpu) == JIT_EXIT_INTERPRET,
              "block from the prior allocation generation was entered");
        CHECK(jit_buf_free(&b), "reallocated entry-guard arena free failed");
    } else {
        printf("  SKIP: host refused entry-guard reallocation\n");
    }
}

static void test_cleanup_of_empty_state(void) {
    jit_buf_t b = {0};
    b.size = 123;
    b.used = 45;
    b.write_depth = 2;
    b.write_owner = 1234;
    CHECK(jit_buf_free(&b), "free with no mapping failed");
    CHECK(is_inactive(&b), "free with no allocation left stale state");

    CHECK(jit_buf_free(NULL), "free(NULL) failed");
    CHECK(!jit_buf_begin_write(NULL), "begin(NULL) succeeded");
    CHECK(!jit_buf_end_write(NULL), "end(NULL) succeeded");
    CHECK(strcmp(jit_buf_policy(NULL), "unallocated") == 0,
          "NULL policy is not unallocated");
}

int main(void) {
    printf("jit memory boundary tests\n");
    test_bad_allocations();
    test_take_and_commit_bounds();
    test_nested_scopes_and_free_cleanup();
    test_overlapping_arenas();
#if TEST_JIT_APPLE_THREAD_WX
    test_wrong_thread_is_rejected();
#endif
    test_block_entry_guards();
    test_cleanup_of_empty_state();
    printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}

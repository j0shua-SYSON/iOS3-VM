/*
 * Host-side tests for the portable C bridge used by the iOS app.
 *
 * These deliberately use a 1 MB synthetic machine.  They exercise the exact
 * address validation and CLCD handoff used on-device without allocating the
 * app's 128 MB demo guest, let alone a real 512 MB XNU machine.
 */
#include "VMGuest.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned tests;
static unsigned failed;

#define CHECK(expr, ...) do {                                                \
    tests++;                                                                 \
    if (!(expr)) {                                                           \
        failed++;                                                            \
        fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
        fprintf(stderr, __VA_ARGS__);                                        \
        fputc('\n', stderr);                                                 \
    }                                                                        \
} while (0)

static void test_framebuffer_address_boundaries(void) {
    const uint32_t minimum = VM_FB_BYTES + VM_GUEST_BLOB_BYTES + 0x10000u;

    CHECK(vm_guest_fb_pa(0x10000000u, minimum - 1u) == 0,
          "undersized RAM bank produced a framebuffer");

    uint32_t pa = vm_guest_fb_pa(0x10000000u, minimum);
    CHECK(pa >= 0x10000000u, "minimum valid bank produced pa=%08x", pa);
    CHECK((pa & 0xfffu) == 0, "framebuffer pa=%08x is not page aligned", pa);
    CHECK((uint64_t)pa + VM_FB_BYTES <= (uint64_t)0x10000000u + minimum,
          "framebuffer extends beyond minimum RAM bank");

    /* A bank ending exactly at 2^32 is representable: its last byte is still
     * a valid 32-bit physical address. */
    pa = vm_guest_fb_pa(0xff000000u, 0x01000000u);
    CHECK(pa >= 0xff000000u, "bank ending at 2^32 was rejected");
    CHECK((uint64_t)pa + VM_FB_BYTES <= 0x100000000ull,
          "top-of-address-space framebuffer overran 2^32");

    CHECK(vm_guest_fb_pa(0xfff00000u, 0x00200000u) == 0,
          "wrapping physical RAM bank was accepted");
}

static void test_null_and_uninitialised_inputs(void) {
    uint32_t w = 1, h = 1, stride = 1;
    vm_pixel_order_t order = VM_ORDER_ARGB;

    CHECK(!vm_guest_install(NULL), "install accepted a null machine");
    CHECK(vm_guest_framebuffer(NULL) == NULL,
          "framebuffer accepted a null machine");
    CHECK(vm_guest_display(NULL, &w, &h, &stride, &order) == NULL,
          "display accepted a null machine");

    s5l8900_t blank;
    memset(&blank, 0, sizeof blank);
    CHECK(!vm_guest_install(&blank), "install accepted a machine without RAM");
    CHECK(vm_guest_framebuffer(&blank) == NULL,
          "framebuffer accepted a machine without RAM");
}

static void test_install_scanout_and_execution(void) {
    s5l8900_t m;
    bool initialised = s5l8900_init(&m, 0, 1u << 20);
    CHECK(initialised, "1 MB machine init failed");
    if (!initialised) return;

    bool installed = vm_guest_install(&m);
    CHECK(installed, "demo guest installation failed");
    if (!installed) { s5l8900_free(&m); return; }
    CHECK(m.cpu.r[15] == m.ram_base, "entry pc=%08x", m.cpu.r[15]);

    const uint32_t expected_pa = vm_guest_fb_pa(m.ram_base, m.ram_size);
    const uint8_t *expected = m.ram + (expected_pa - m.ram_base);
    CHECK(vm_guest_framebuffer(&m) == expected,
          "fixed framebuffer pointer disagrees with physical address");

    uint32_t w = 0, h = 0, stride = 0;
    vm_pixel_order_t order = VM_ORDER_ARGB;
    const uint8_t *display = vm_guest_display(&m, &w, &h, &stride, &order);
    CHECK(display == expected, "CLCD scanout did not select the demo buffer");
    CHECK(w == VM_FB_WIDTH && h == VM_FB_HEIGHT,
          "scanout geometry=%ux%u", w, h);
    CHECK(stride == VM_FB_WIDTH * VM_FB_BPP,
          "scanout stride=%u", stride);
    CHECK(order == VM_ORDER_BGRA, "demo scanout order=%u", (unsigned)order);

    arm_status_t status = ARM_OK;
    unsigned retired = s5l8900_run(&m, 2000, &status);
    CHECK(retired == 2000 && status == ARM_OK,
          "demo stopped early: retired=%u status=%d", retired, (int)status);
    CHECK(m.uart0.tx_len >= strlen("iOS3-VM:"),
          "demo did not publish its UART banner");
    CHECK(memcmp(m.uart0.tx, "iOS3-VM:", strlen("iOS3-VM:")) == 0,
          "unexpected UART prefix");
    CHECK(expected[0] == 0 && expected[1] == 0 &&
          expected[2] == 0 && expected[3] == 0xff,
          "first BGRA pixel was %02x %02x %02x %02x",
          expected[0], expected[1], expected[2], expected[3]);

    s5l8900_free(&m);
}

static void test_host_refuses_malicious_clcd_windows(void) {
    s5l8900_t m;
    bool initialised = s5l8900_init(&m, 0, 1u << 20);
    CHECK(initialised, "1 MB machine init failed");
    if (!initialised) return;
    bool installed = vm_guest_install(&m);
    CHECK(installed, "demo guest installation failed");
    if (!installed) { s5l8900_free(&m); return; }

    const uint32_t fixed_pa = vm_guest_fb_pa(m.ram_base, m.ram_size);
    const uint8_t *fixed = m.ram + (fixed_pa - m.ram_base);
    const uint32_t b = CLCD_WIN_FIRST;

    /* Keep the window enabled and apparently well-formed, but place its last
     * line outside RAM.  The app must fall back instead of handing UIKit an
     * out-of-bounds host pointer. */
    s5l_clcd_write(&m.clcd, b + CLCD_WIN_FBADDR,
                   m.ram_base + m.ram_size - 4u);
    CHECK(vm_guest_display(&m, NULL, NULL, NULL, NULL) == fixed,
          "out-of-RAM CLCD window escaped validation");

    CHECK(s5l_clcd_seed_window0(&m.clcd, fixed_pa,
                                VM_FB_WIDTH, VM_FB_HEIGHT,
                                VM_FB_WIDTH * VM_FB_BPP,
                                CLCD_FMT_32BPP, CLCD_ORDER_BGRA),
          "could not restore a valid CLCD window");
    /* The seed API rejects this layout, but guest MMIO is allowed to program
     * arbitrary register bits. Exercise the latter trust boundary directly. */
    s5l_clcd_write(&m.clcd, b + CLCD_WIN_PITCH, UINT32_MAX);
    CHECK(vm_guest_display(&m, NULL, NULL, NULL, NULL) == fixed,
          "overflowing CLCD stride escaped validation");

    CHECK(s5l_clcd_seed_window0(&m.clcd, fixed_pa,
                                VM_FB_WIDTH, VM_FB_HEIGHT,
                                VM_FB_WIDTH * VM_FB_BPP,
                                CLCD_FMT_32BPP, CLCD_ORDER_ARGB),
          "could not seed a valid ARGB window");
    vm_pixel_order_t order = VM_ORDER_BGRA;
    CHECK(vm_guest_display(&m, NULL, NULL, NULL, &order) == fixed,
          "valid ARGB window was rejected");
    CHECK(order == VM_ORDER_ARGB, "ARGB order was not propagated");

    s5l8900_free(&m);
}

int main(void) {
    test_framebuffer_address_boundaries();
    test_null_and_uninitialised_inputs();
    test_install_scanout_and_execution();
    test_host_refuses_malicious_clcd_windows();

    printf("vmguest: %u checks, %u failed\n", tests, failed);
    return failed ? 1 : 0;
}

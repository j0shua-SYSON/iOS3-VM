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
    CHECK(w == 0 && h == 0 && stride == 0 && order == VM_ORDER_BGRA,
          "failed display lookup left stale output metadata");

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
     * line outside RAM. The app must reject it instead of handing UIKit an
     * out-of-bounds host pointer or hiding the failure behind a stale frame. */
    s5l_clcd_write(&m.clcd, b + CLCD_WIN_FBADDR,
                   m.ram_base + m.ram_size - 4u);
    CHECK(vm_guest_display(&m, NULL, NULL, NULL, NULL) == NULL,
          "out-of-RAM CLCD window escaped validation");

    CHECK(s5l_clcd_seed_window0(&m.clcd, fixed_pa,
                                VM_FB_WIDTH, VM_FB_HEIGHT,
                                VM_FB_WIDTH * VM_FB_BPP,
                                CLCD_FMT_32BPP, CLCD_ORDER_BGRA),
          "could not restore a valid CLCD window");
    /* The seed API rejects this layout, but guest MMIO is allowed to program
     * arbitrary register bits. Exercise the latter trust boundary directly. */
    s5l_clcd_write(&m.clcd, b + CLCD_WIN_PITCH, UINT32_MAX);
    CHECK(vm_guest_display(&m, NULL, NULL, NULL, NULL) == NULL,
          "overflowing CLCD stride escaped validation");

    for (uint32_t rawOrder = 0; rawOrder <= CLCD_ORDER_MASK; rawOrder++) {
        CHECK(s5l_clcd_seed_window0(&m.clcd, fixed_pa,
                                    VM_FB_WIDTH, VM_FB_HEIGHT,
                                    VM_FB_WIDTH * VM_FB_BPP,
                                    CLCD_FMT_32BPP, rawOrder),
              "could not seed valid order value %u", rawOrder);
        vm_pixel_order_t order = VM_ORDER_ARGB;
        CHECK(vm_guest_display(&m, NULL, NULL, NULL, &order) == fixed,
              "valid order value %u was rejected", rawOrder);
        CHECK(order == VM_ORDER_BGRA,
              "unverified order value %u invented a swizzle", rawOrder);
    }

    s5l8900_free(&m);
}

static void test_host_follows_active_clcd_window(void) {
    s5l8900_t m;
    bool initialised = s5l8900_init(&m, 0x08000000u, 2u << 20);
    CHECK(initialised, "2 MB machine init failed");
    if (!initialised) return;

    const uint32_t fb0 = m.ram_base + 0x100000u;
    const uint32_t fb1 = m.ram_base + 0x160000u;
    CHECK(s5l_clcd_seed_window0(&m.clcd, fb0,
                                VM_FB_WIDTH, VM_FB_HEIGHT,
                                VM_FB_WIDTH * VM_FB_BPP,
                                CLCD_FMT_32BPP, CLCD_ORDER_BGRA),
          "could not seed window 0");

    const uint32_t b1 = CLCD_WIN_FIRST + CLCD_WIN_STRIDE;
    s5l_clcd_write(&m.clcd, b1 + CLCD_WIN_PITCH, VM_FB_WIDTH * VM_FB_BPP);
    s5l_clcd_write(&m.clcd, b1 + CLCD_WIN_CONTROL,
                   (6u << CLCD_FMT_SHIFT) /* driver also defines format 6 as 32 bpp */
                   | (2u << CLCD_ORDER_SHIFT));
    s5l_clcd_write(&m.clcd, b1 + CLCD_WIN_FBADDR, fb1);
    s5l_clcd_write(&m.clcd, b1 + CLCD_WIN_GEOMETRY,
                   (VM_FB_WIDTH << 16) | VM_FB_HEIGHT);

    /* Window 0 has priority while both are enabled. */
    s5l_clcd_write(&m.clcd, CLCD_CTRL,
                   CLCD_CTRL_ENABLE | CLCD_CTRL_WIN0 | CLCD_CTRL_WIN1);
    CHECK(vm_guest_display(&m, NULL, NULL, NULL, NULL) ==
          m.ram + (fb0 - m.ram_base),
          "window priority did not select window 0");

    /* Once the guest disables window 0, the host must follow window 1. */
    s5l_clcd_write(&m.clcd, CLCD_CTRL,
                   CLCD_CTRL_ENABLE | CLCD_CTRL_WIN1);
    vm_pixel_order_t order = VM_ORDER_ARGB;
    CHECK(vm_guest_display(&m, NULL, NULL, NULL, &order) ==
          m.ram + (fb1 - m.ram_base),
          "active window 1 did not become scanout");
    CHECK(order == VM_ORDER_BGRA, "unknown order bits invented a swizzle");

    s5l_clcd_write(&m.clcd, CLCD_DISABLE, 1u);
    CHECK(vm_guest_display(&m, NULL, NULL, NULL, NULL) == NULL,
          "stopped scanout still exposed a framebuffer");
    s5l_clcd_write(&m.clcd, CLCD_ENABLE, 1u);

    /* No enabled window is an explicit no-scanout state, not the demo buffer. */
    s5l_clcd_write(&m.clcd, CLCD_CTRL, CLCD_CTRL_ENABLE);
    CHECK(vm_guest_display(&m, NULL, NULL, NULL, NULL) == NULL,
          "disabled windows produced a stale fallback frame");

    s5l8900_free(&m);
}

int main(void) {
    test_framebuffer_address_boundaries();
    test_null_and_uninitialised_inputs();
    test_install_scanout_and_execution();
    test_host_refuses_malicious_clcd_windows();
    test_host_follows_active_clcd_window();

    printf("vmguest: %u checks, %u failed\n", tests, failed);
    return failed ? 1 : 0;
}

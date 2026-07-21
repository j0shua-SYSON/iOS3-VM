# The emulator core: portable C11, zero platform dependencies.
# This same static library is what the iOS app links against.
add_library(emucore STATIC
    src/arm/arm_interp.c
    src/arm/vfp.c
    src/mmu.c
    src/snapshot.c
    src/vm_source.c
    src/vm_block.c
    src/soc/uart.c
    src/soc/vic.c
    src/soc/timer.c
    src/soc/power.c
    src/soc/clcd.c
    src/soc/nor.c
    src/soc/nand.c
    src/soc/storage.c
    src/soc/machine.c
    src/firmware/img3.c
    src/firmware/aes.c
    src/firmware/loader.c
    src/firmware/devicetree.c
    src/firmware/lzss.c
    src/firmware/macho.c
    src/firmware/ksyms.c
)
target_include_directories(emucore PUBLIC include)

# vfp.c uses sqrt/sqrtf/trunc and <fenv.h>'s feclearexcept/fetestexcept to sample
# the IEEE sticky flags. MinGW and macOS fold libm into libc, so this only shows
# up as an undefined-reference wall of text when linking on Linux — which is why
# it passed on the dev box and broke CI. PUBLIC so every consumer of emucore
# inherits it.
if(NOT WIN32 AND NOT APPLE)
    target_link_libraries(emucore PUBLIC m)
endif()

if(NOT MSVC)
    target_compile_options(emucore PRIVATE -Wall -Wextra)
    # VFPv2 has no fused multiply-add: VMLA rounds the product and then rounds
    # the sum. GCC's default -ffp-contract=fast is allowed to fuse those into
    # one FMA, which differs from hardware in the last bit. vfp.c also anchors
    # each rounding step behind a volatile store, so this flag is a second
    # belt rather than the only one — but a last-bit difference in a float is
    # exactly the silent wrongness this core refuses to ship.
    set_source_files_properties(src/arm/vfp.c PROPERTIES
        COMPILE_OPTIONS "-ffp-contract=off")
endif()

# --- dynamic recompiler (optional, off by default) --------------------------
# See docs/dynarec.md. The translator is a layer over the interpreter: anything
# it does not handle is left to arm_step(), so enabling this cannot change what
# the guest computes, only how fast. It is still off by default because nothing
# in the machine's run loop calls it yet.
if(IOS3VM_JIT)
    target_sources(emucore PRIVATE
        src/jit/a64_emit.c
        src/jit/jit_translate.c
        src/jit/jit_mem.c
    )
    target_compile_definitions(emucore PUBLIC IOS3VM_JIT)

    # Byte-exact encoder tests. Pure computation, so these run everywhere —
    # including the x86 dev box, which is the point.
    add_executable(test_a64_emit tests/test_a64_emit.c)
    target_link_libraries(test_a64_emit PRIVATE emucore)
    add_test(NAME a64_emitter COMMAND test_a64_emit)

    # Block-shape and emitted-code tests. Also host-independent.
    add_executable(test_jit tests/test_jit.c)
    target_link_libraries(test_jit PRIVATE emucore)
    add_test(NAME jit_translator COMMAND test_jit)

    # Boundary tests for the host-memory shim. Build jit_mem.c directly so this
    # target stays tiny and does not pull in the translator or machine core.
    add_executable(test_jit_mem tests/test_jit_mem.c src/jit/jit_mem.c)
    target_include_directories(test_jit_mem PRIVATE include)
    add_test(NAME jit_memory COMMAND test_jit_mem)

    # The only test that executes emitted code. It exits successfully with a
    # SKIP verdict unless the host is arm64 and executable memory is available,
    # so it is safe to register everywhere. Hosted CI additionally requires the
    # macOS runners to execute through MAP_JIT rather than skip or fall back.
    add_executable(test_jit_exec tests/test_jit_exec.c)
    target_link_libraries(test_jit_exec PRIVATE emucore)
    add_test(NAME jit_execution COMMAND test_jit_exec)
    # Deliberately do not set PASS_ or SKIP_REGULAR_EXPRESSION: either can
    # override a non-zero process result. The workflow separately validates
    # that macOS really executed MAP_JIT code instead of taking a skip path.
endif()

# --- unit tests -------------------------------------------------------------
add_executable(test_arm tests/test_arm.c)
target_link_libraries(test_arm PRIVATE emucore)
add_test(NAME arm_interp COMMAND test_arm)

add_executable(test_vfp tests/test_vfp.c)
target_link_libraries(test_vfp PRIVATE emucore)
add_test(NAME vfp COMMAND test_vfp)

add_executable(test_soc tests/test_soc.c)
target_link_libraries(test_soc PRIVATE emucore)
add_test(NAME s5l8900_machine COMMAND test_soc)

add_executable(test_img3 tests/test_img3.c)
target_link_libraries(test_img3 PRIVATE emucore)
add_test(NAME img3_parser COMMAND test_img3)

add_executable(test_aes tests/test_aes.c)
target_link_libraries(test_aes PRIVATE emucore)
add_test(NAME aes_fips197 COMMAND test_aes)

add_executable(test_loader tests/test_loader.c)
target_link_libraries(test_loader PRIVATE emucore)
add_test(NAME firmware_loader COMMAND test_loader)

add_executable(test_devicetree tests/test_devicetree.c)
target_link_libraries(test_devicetree PRIVATE emucore)
add_test(NAME device_tree COMMAND test_devicetree)

add_executable(test_lzss tests/test_lzss.c)
target_link_libraries(test_lzss PRIVATE emucore)
add_test(NAME lzss COMMAND test_lzss)

add_executable(test_ksyms tests/test_ksyms.c)
target_link_libraries(test_ksyms PRIVATE emucore)
add_test(NAME ksyms COMMAND test_ksyms)

add_executable(test_snapshot tests/test_snapshot.c)
target_link_libraries(test_snapshot PRIVATE emucore)
add_test(NAME snapshot COMMAND test_snapshot)

add_executable(test_nand tests/test_nand.c)
target_link_libraries(test_nand PRIVATE emucore)
add_test(NAME nand_device COMMAND test_nand)

add_executable(test_vm_source tests/test_vm_source.c)
target_link_libraries(test_vm_source PRIVATE emucore)
add_test(NAME vm_source COMMAND test_vm_source)

add_executable(test_vm_block tests/test_vm_block.c)
target_link_libraries(test_vm_block PRIVATE emucore)
add_test(NAME vm_block COMMAND test_vm_block)

# VMGuest is the portable-C seam between the iOS shell and emucore.  Building
# it only with Xcode left its physical-address and scanout bounds checks outside
# the sanitizer suite, even though a mistake there becomes an out-of-bounds
# framebuffer read on the phone.  Keep this target host-only and tiny: it does
# not compile UIKit or allocate the real 128/512 MB guest configuration.
add_executable(test_vmguest
    ../app/Tests/test_vmguest.c
    ../app/Sources/VMGuest.c
)
target_include_directories(test_vmguest PRIVATE ../app/Sources)
target_link_libraries(test_vmguest PRIVATE emucore)
add_test(NAME app_vmguest_bridge COMMAND test_vmguest)

# --- host tools -------------------------------------------------------------
# img3dump points the real parser at real firmware, so inspecting an IPSW also
# validates our format handling.
add_executable(img3dump ../tools/img3dump.c)
target_link_libraries(img3dump PRIVATE emucore)

# runfw executes a decrypted payload on the emulated machine and reports exactly
# where and why it stopped — the diagnostic that says what to build next.
add_executable(runfw ../tools/runfw.c)
target_link_libraries(runfw PRIVATE emucore)

add_executable(unlzss ../tools/unlzss.c)
target_link_libraries(unlzss PRIVATE emucore)

add_executable(machoinfo ../tools/machoinfo.c)
target_link_libraries(machoinfo PRIVATE emucore)

add_executable(bootkernel ../tools/bootkernel.c)
target_link_libraries(bootkernel PRIVATE emucore)

# snapboot is the snapshot acceptance harness: it boots to a checkpoint, saves,
# and can restore into a fresh process. Comparing snapshot bytes alone is not
# enough — a field the format never stores is absent from both sides and
# cancels out — so it also prints a machine-derived report to diff.
add_executable(snapboot ../tools/snapboot.c)
target_link_libraries(snapboot PRIVATE emucore)

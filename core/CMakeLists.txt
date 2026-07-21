# The emulator core: portable C11, zero platform dependencies.
# This same static library is what the iOS app links against.
add_library(emucore STATIC
    src/arm/arm_interp.c
    src/mmu.c
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
)
target_include_directories(emucore PUBLIC include)

if(NOT MSVC)
    target_compile_options(emucore PRIVATE -Wall -Wextra)
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

    # The only test that executes emitted code. Skips itself (exit 0) unless
    # the host is arm64 AND executable memory can be obtained, so it is safe to
    # register unconditionally: on x86 it reports SKIP, on the GitHub Actions
    # macos-14/macos-15 Apple Silicon runners it really runs.
    add_executable(test_jit_exec tests/test_jit_exec.c)
    target_link_libraries(test_jit_exec PRIVATE emucore)
    add_test(NAME jit_execution COMMAND test_jit_exec)
    set_tests_properties(jit_execution PROPERTIES
        PASS_REGULAR_EXPRESSION "SKIP:|0 failed")
endif()

# --- unit tests -------------------------------------------------------------
add_executable(test_arm tests/test_arm.c)
target_link_libraries(test_arm PRIVATE emucore)
add_test(NAME arm_interp COMMAND test_arm)

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

add_executable(test_nand tests/test_nand.c)
target_link_libraries(test_nand PRIVATE emucore)
add_test(NAME nand_device COMMAND test_nand)

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

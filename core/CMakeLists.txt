# The emulator core: portable C11, zero platform dependencies.
# This same static library is what the iOS app links against.
add_library(emucore STATIC
    src/arm/arm_interp.c
    src/arm/vfp.c
    src/mmu.c
    src/snapshot.c
    src/vm_source.c
    src/vm_block.c
    src/md_bridge.c
    src/md_raw_bridge.c
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

add_executable(test_md_bridge tests/test_md_bridge.c)
target_link_libraries(test_md_bridge PRIVATE emucore)
add_test(NAME host_memory_disk_bridge COMMAND test_md_bridge)

add_executable(test_md_raw_bridge tests/test_md_raw_bridge.c)
target_link_libraries(test_md_raw_bridge PRIVATE emucore)
add_test(NAME host_raw_memory_disk_bridge COMMAND test_md_raw_bridge)

add_executable(test_macho tests/test_macho.c)
target_link_libraries(test_macho PRIVATE emucore)
add_test(NAME macho_parser COMMAND test_macho)

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
# The host-owned writable-file adapter stays outside emucore: the portable core
# exposes vm_block_t callbacks but never depends on descriptors, locks, or
# platform sync APIs.  Build this small boundary with the strictest warnings.
add_library(host_file_block STATIC ../tools/file_block.c)
target_include_directories(host_file_block PUBLIC ../tools include)
set_target_properties(host_file_block PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(NOT WIN32)
    target_compile_definitions(host_file_block PRIVATE _FILE_OFFSET_BITS=64)
endif()
if(MSVC)
    target_compile_options(host_file_block PRIVATE /W4 /WX)
else()
    target_compile_options(host_file_block PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

add_executable(test_file_block tests/test_file_block.c)
target_link_libraries(test_file_block PRIVATE emucore host_file_block)
set_target_properties(test_file_block PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    # Fixture helpers deliberately use ISO fopen; contain the secure-CRT opt-out
    # to this test instead of weakening warnings for production targets.
    target_compile_definitions(test_file_block PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_file_block PRIVATE /W4 /WX)
else()
    target_compile_options(test_file_block PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME file_block_adapter COMMAND test_file_block)
set_tests_properties(file_block_adapter PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

# Kernel compatibility patches are a host policy, not a property of the CPU
# core.  Keep their bounded, firmware-neutral transaction helper outside
# emucore so a frontend must opt into a version-specific manifest explicitly.
add_library(host_guest_patch STATIC ../tools/guest_patch.c)
target_include_directories(host_guest_patch PUBLIC ../tools)
set_target_properties(host_guest_patch PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(host_guest_patch PRIVATE /W4 /WX)
else()
    target_compile_options(host_guest_patch PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

add_executable(test_guest_patch tests/test_guest_patch.c)
target_link_libraries(test_guest_patch PRIVATE host_guest_patch)
set_target_properties(test_guest_patch PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_guest_patch PRIVATE /W4 /WX)
else()
    target_compile_options(test_guest_patch PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME atomic_guest_patch COMMAND test_guest_patch)

# The exact kernel and rootfs policies authenticate caller-selected bytes with
# the same small, allocation-free implementation.  Keep that primitive in a
# separate strict target so neither host policy carries a private hash copy.
add_library(host_sha256 STATIC ../tools/sha256.c)
target_include_directories(host_sha256 PUBLIC ../tools)
set_target_properties(host_sha256 PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(host_sha256 PRIVATE /W4 /WX)
else()
    target_compile_options(host_sha256 PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

# The exact iPhone OS 3.1.3 kernel manifest is a second host-policy layer over
# the firmware-neutral transaction helper.  Keeping it in its own target makes
# the supported build identity explicit and prevents the portable CPU core from
# silently accepting version-specific patches.
add_library(host_ios3_kernel_patch STATIC ../tools/ios3_kernel_patch.c)
target_include_directories(host_ios3_kernel_patch PUBLIC ../tools include)
target_link_libraries(host_ios3_kernel_patch
    PUBLIC host_guest_patch host_sha256
    PRIVATE emucore)
set_target_properties(host_ios3_kernel_patch PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(host_ios3_kernel_patch PRIVATE /W4 /WX)
else()
    target_compile_options(host_ios3_kernel_patch PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

add_executable(test_ios3_kernel_patch tests/test_ios3_kernel_patch.c)
target_link_libraries(test_ios3_kernel_patch PRIVATE host_ios3_kernel_patch)
set_target_properties(test_ios3_kernel_patch PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_ios3_kernel_patch PRIVATE /W4 /WX)
else()
    target_compile_options(test_ios3_kernel_patch PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME exact_ios3_kernel_patch COMMAND test_ios3_kernel_patch)

# Provisioning a writable rootfs image is host filesystem work, not emulated
# machine behavior.  The implementation is intentionally standalone and uses
# at most a 1 MiB transfer buffer, so its format and publication tests remain
# cheap on every hosted runner.
add_library(host_rootfs_work STATIC ../tools/rootfs_work.c)
target_include_directories(host_rootfs_work PUBLIC ../tools)
target_link_libraries(host_rootfs_work PUBLIC host_sha256)
set_target_properties(host_rootfs_work PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(host_rootfs_work PRIVATE /W4 /WX)
else()
    target_compile_options(host_rootfs_work PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()

add_executable(test_rootfs_work tests/test_rootfs_work.c)
target_link_libraries(test_rootfs_work PRIVATE host_rootfs_work)
set_target_properties(test_rootfs_work PROPERTIES
    C_STANDARD 11
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_rootfs_work PRIVATE /W4 /WX)
else()
    target_compile_options(test_rootfs_work PRIVATE
        -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME bounded_rootfs_work_image COMMAND test_rootfs_work)
set_tests_properties(bounded_rootfs_work_image PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

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
target_link_libraries(bootkernel PRIVATE
    emucore host_file_block host_guest_patch host_ios3_kernel_patch
    host_rootfs_work)

# Public CI cannot contain Apple's firmware, but it can still exercise the
# external-md parser's fail-closed boundary before any file is opened. These
# cases protect the combinations that would otherwise mix incompatible storage
# or checkpoint ownership models.
add_test(NAME external_md_cli_preflight
    COMMAND ${CMAKE_COMMAND}
            -DBOOTKERNEL=$<TARGET_FILE:bootkernel>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_bootkernel_external_md.cmake)

# snapboot is the snapshot acceptance harness: it boots to a checkpoint, saves,
# and can restore into a fresh process. Comparing snapshot bytes alone is not
# enough — a field the format never stores is absent from both sides and
# cancels out — so it also prints a machine-derived report to diff.
add_executable(snapboot ../tools/snapboot.c)
target_link_libraries(snapboot PRIVATE emucore)

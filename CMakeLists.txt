# iOS3-VM — top-level build.
# Builds and tests the platform-independent emulator core on any host
# (Windows/Linux/macOS). The iOS app is built separately by Xcode; see app/.
cmake_minimum_required(VERSION 3.16)
project(ios3vm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Default to an optimised build. The interpreter is the hot loop of everything
# here — running the real kernel is hundreds of millions of instructions — and
# the committed build carried no -O at all, leaving a measured ~3x on the floor.
# Optimising also earns its keep as a correctness check: if -O2 and -O0 ever
# disagree, that is a real bug (usually undefined behaviour) worth catching, and
# CI builds both. Overridable with -DCMAKE_BUILD_TYPE=Debug.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# The ARMv6 -> arm64 dynamic recompiler (docs/dynarec.md). OFF by default and
# it must stay that way: the interpreter is the specification and the shipping
# engine, and nothing about the default build or the boot path changes when
# this is off. Turning it on adds three source files to emucore and four test
# executables; the emitter and translator are portable C and are fully tested
# on any host, while core/tests/test_jit_exec.c — the only test that *runs*
# emitted code — skips itself at runtime unless the host is arm64.
option(IOS3VM_JIT "Build the ARMv6 -> arm64 dynamic recompiler" OFF)

enable_testing()
add_subdirectory(core)

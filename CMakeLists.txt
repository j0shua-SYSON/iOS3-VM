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

enable_testing()
add_subdirectory(core)

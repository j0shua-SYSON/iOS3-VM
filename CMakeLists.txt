# iOS3-VM — top-level build.
# Builds and tests the platform-independent emulator core on any host
# (Windows/Linux/macOS). The iOS app is built separately by Xcode; see app/.
cmake_minimum_required(VERSION 3.16)
project(ios3vm C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

enable_testing()
add_subdirectory(core)

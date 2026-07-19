# The emulator core: portable C11, zero platform dependencies.
# This same static library is what the iOS app links against.
add_library(emucore STATIC
    src/arm/arm_interp.c
)
target_include_directories(emucore PUBLIC include)

if(NOT MSVC)
    target_compile_options(emucore PRIVATE -Wall -Wextra)
endif()

# --- unit tests -------------------------------------------------------------
add_executable(test_arm tests/test_arm.c)
target_link_libraries(test_arm PRIVATE emucore)
add_test(NAME arm_interp COMMAND test_arm)

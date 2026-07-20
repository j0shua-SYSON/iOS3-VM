# The emulator core: portable C11, zero platform dependencies.
# This same static library is what the iOS app links against.
add_library(emucore STATIC
    src/arm/arm_interp.c
    src/mmu.c
    src/soc/uart.c
    src/soc/vic.c
    src/soc/timer.c
    src/soc/nor.c
    src/soc/nand.c
    src/soc/storage.c
    src/soc/machine.c
    src/firmware/img3.c
    src/firmware/aes.c
    src/firmware/loader.c
    src/firmware/devicetree.c
)
target_include_directories(emucore PUBLIC include)

if(NOT MSVC)
    target_compile_options(emucore PRIVATE -Wall -Wextra)
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

add_executable(test_nand tests/test_nand.c)
target_link_libraries(test_nand PRIVATE emucore)
add_test(NAME nand_device COMMAND test_nand)
